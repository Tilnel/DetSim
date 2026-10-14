#include "state_store.h"
#include "cereal/archives/binary.hpp"
#include "debug.h"
#include "state.h"
#include "state_store_packed.h"
#include "sysstate_store.h"
#include "xxhash.h"
#include "utils.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <stdexcept>

StateStore::StateStore() = default;
StateStore::~StateStore() {
  try { shutdown(); }
  catch (const std::exception &error) { LOG_ERROR("StateStore shutdown: %s", error.what()); }
}
StateStore &StateStore::instance() {
  // The packed store must outlive our destructor and background threads.
  StateStorePacked::instance();
  static StateStore store;
  return store;
}
void StateStore::init() { init(Config{}); }
void StateStore::init(const Config &config) {
  std::unique_lock lifecycle(lifecycle_mutex_);
  if (initialized_) return;
  if (config.io_threads <= 0 || config.max_pending_bytes == 0)
    throw std::invalid_argument("StateStore needs an I/O worker and a nonzero pending-byte limit");
  StateStorePacked::Config packed;
  packed.storage_path = config.packed_storage_path;
  packed.compression_level = config.compression_level;
  packed.read_only = config.read_only;
  StateStorePacked::instance().init(packed);
  {
    std::lock_guard lock(mutex_);
    config_ = config;
    stopping_ = false;
    failure_ = nullptr;
    stats_ = {};
  }
  try {
    for (int i = 0; i < config_.io_threads; ++i)
      io_threads_.emplace_back(&StateStore::io_worker, this);
    for (size_t i = 0; i < config_.prefetch_threads; ++i)
      prefetch_threads_.emplace_back(&StateStore::prefetch_worker, this);
  } catch (...) {
    { std::lock_guard lock(mutex_); stopping_ = true; }
    work_cv_.notify_all(); prefetch_cv_.notify_all();
    for (auto &thread : io_threads_) thread.join();
    for (auto &thread : prefetch_threads_) thread.join();
    io_threads_.clear(); prefetch_threads_.clear();
    StateStorePacked::instance().shutdown();
    throw;
  }
  { std::lock_guard lock(mutex_); initialized_ = true; }
  ever_initialized_ = true;
}
void StateStore::ensure_initialized() {
  if (!ever_initialized_.load()) init();
}
void StateStore::check_ready() const {
  if (failure_) std::rethrow_exception(failure_);
  if (!initialized_ || stopping_) throw std::runtime_error("StateStore is not running");
}
void StateStore::record_failure(std::exception_ptr failure) {
  if (!failure_) failure_ = failure;
  // Every waiter observes the same failure; no dropped write reports success.
  completion_cv_.notify_all(); space_cv_.notify_all(); work_cv_.notify_all();
  prefetch_cv_.notify_all();
}
void StateStore::shutdown() {
  std::unique_lock lifecycle(lifecycle_mutex_);
  if (!initialized_) return;
  { std::lock_guard lock(mutex_); stopping_ = true; }
  work_cv_.notify_all(); prefetch_cv_.notify_all();
  for (auto &thread : io_threads_) thread.join();
  for (auto &thread : prefetch_threads_) thread.join();
  io_threads_.clear(); prefetch_threads_.clear();
  auto failure = failure_;
  try { StateStorePacked::instance().shutdown(); }
  catch (...) { if (!failure) failure = std::current_exception(); }
  {
    std::lock_guard lock(mutex_);
    pending_.clear(); pending_bytes_ = 0; io_queue_.clear();
    hot_cache_.clear(); warm_cache_.clear(); hot_lru_.clear(); warm_lru_.clear();
    hot_memory_usage_ = 0; warm_memory_usage_ = 0;
    prefetch_inflight_.clear(); prefetch_queue_.clear(); state_queue_.clear();
    initialized_ = false;
  }
  if (failure) std::rethrow_exception(failure);
}

StateStore::Buffer StateStore::find_cache(Cache &cache, std::list<hash_type> &lru,
                                         hash_type hash) {
  auto found = cache.find(hash);
  if (found == cache.end()) return {};
  lru.splice(lru.begin(), lru, found->second.position);
  return found->second.bytes;
}
void StateStore::insert_cache(Cache &cache, std::list<hash_type> &lru,
                              std::atomic<size_t> &usage, size_t capacity,
                              hash_type hash, Buffer bytes, size_t original_size) {
  if (bytes->size() > capacity || cache.contains(hash)) return;
  // Erasing cache ownership never mutates bytes retained by readers or I/O.
  while (usage.load() > capacity - bytes->size()) {
    auto victim = cache.find(lru.back());
    auto size = victim->second.bytes->size();
    usage -= size; cache.erase(victim); lru.pop_back();
    ++stats_.evict_calls; stats_.evict_bytes += size;
  }
  lru.push_front(hash);
  try { cache.emplace(hash, CacheEntry{bytes, original_size, lru.begin()}); }
  catch (...) { lru.pop_front(); throw; }
  usage += bytes->size();
}

hash_type StateStore::save(const void *data, size_t len) {
  if (!data || !len) throw std::invalid_argument("empty snapshot");
  if (len > UINT32_MAX) throw std::length_error("snapshot exceeds packed record limit");
  ensure_initialized();
  std::shared_lock lifecycle(lifecycle_mutex_);
  const auto started = std::chrono::steady_clock::now();
  const auto hash = XXHash64::hash(data, len);
  if (!hash) throw std::runtime_error("snapshot hash is reserved zero");
  std::unique_lock lock(mutex_);
  check_ready();
  if (config_.read_only) throw std::runtime_error("StateStore is read-only");
  ++stats_.save_calls;
  for (;;) {
    check_ready();
    Buffer existing;
    auto pending = pending_.find(hash);
    if (pending != pending_.end()) existing = pending->second;
    if (!existing) existing = find_cache(hot_cache_, hot_lru_, hash);
    if (existing) {
      if (existing->size() != len || memcmp(existing->data(), data, len))
        throw std::runtime_error("snapshot hash collision");
      ++stats_.save_dedup_hot; return hash;
    }
    // Compare duplicate content, including disk-only entries: hashes are keys,
    // not permission to silently discard different state bytes.
    if (auto compressed = find_cache(warm_cache_, warm_lru_, hash)) {
      auto bytes = StateStorePacked::instance().decompress(*compressed, warm_cache_.at(hash).original_size);
      if (bytes.size() != len || memcmp(bytes.data(), data, len))
        throw std::runtime_error("snapshot hash collision");
      ++stats_.save_dedup_warm; return hash;
    }
    if (StateStorePacked::instance().exists(hash)) {
      std::vector<uint8_t> bytes;
      if (StateStorePacked::instance().load(hash, bytes) < 0 || bytes.size() != len || memcmp(bytes.data(), data, len))
        throw std::runtime_error("snapshot hash collision or unreadable existing state");
      ++stats_.save_dedup_disk; return hash;
    }
    // Bound all accepted unfinished writes, including dequeued/in-flight work.
    if (pending_.empty() || (pending_.size() < max_io_queue_size_ &&
        len <= config_.max_pending_bytes && pending_bytes_ <= config_.max_pending_bytes - len)) break;
    space_cv_.wait(lock);
  }
  auto bytes = std::make_shared<const std::vector<uint8_t>>(
      static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + len);
  pending_.emplace(hash, bytes);
  try { io_queue_.push_back(hash); }
  catch (...) { pending_.erase(hash); throw; }
  pending_bytes_ += len;
  // Cache insertion is optional; accepted persistence must still run if it fails.
  try { insert_cache(hot_cache_, hot_lru_, hot_memory_usage_, config_.hot_cache_size, hash, bytes, len); }
  catch (...) { work_cv_.notify_one(); throw; }
  ++stats_.save_new;
  auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started).count();
  stats_.save_time_us += elapsed; stats_.save_new_slow += elapsed > 1000;
  work_cv_.notify_one();
  return hash;
}
void StateStore::io_worker() {
  for (;;) {
    hash_type hash;
    Buffer bytes;
    {
      std::unique_lock lock(mutex_);
      work_cv_.wait(lock, [&] {return stopping_ || failure_ || !io_queue_.empty();});
      if (failure_ || (stopping_ && io_queue_.empty())) return;
      hash = io_queue_.front(); io_queue_.pop_front();
      bytes = pending_.at(hash);
    }
    try {
      auto &packed = StateStorePacked::instance();
      auto compressed = std::make_shared<const std::vector<uint8_t>>(packed.compress(*bytes));
      if (compressed->empty() || packed.save(compressed->data(), compressed->size(), hash, true, bytes->size()) != hash)
        throw std::runtime_error("snapshot persistence failed");
      std::lock_guard lock(mutex_);
      insert_cache(warm_cache_, warm_lru_, warm_memory_usage_, config_.warm_cache_size, hash, compressed, bytes->size());
      pending_bytes_ -= bytes->size(); pending_.erase(hash);
      space_cv_.notify_all(); completion_cv_.notify_all();
    } catch (...) {
      std::lock_guard lock(mutex_); record_failure(std::current_exception()); return;
    }
  }
}
void StateStore::wait_for_completion() {
  ensure_initialized();
  std::shared_lock lifecycle(lifecycle_mutex_);
  std::unique_lock lock(mutex_);
  check_ready();
  completion_cv_.wait(lock, [&] {return failure_ || pending_.empty();});
  check_ready();
  // Holding mutex_ excludes new accepted writes until both data and index sync.
  try { StateStorePacked::instance().flush_index(); }
  catch (...) { record_failure(std::current_exception()); throw; }
}

StateStore::Buffer StateStore::load_buffer(hash_type hash) {
  if (!hash) return {};
  Buffer compressed;
  size_t original_size = 0;
  {
    std::lock_guard lock(mutex_); check_ready(); ++stats_.load_requests;
    if (auto bytes = find_cache(hot_cache_, hot_lru_, hash)) {++stats_.l1_hits; return bytes;}
    auto pending = pending_.find(hash);
    if (pending != pending_.end()) {++stats_.l1_hits; return pending->second;}
    compressed = find_cache(warm_cache_, warm_lru_, hash);
    if (compressed) {original_size = warm_cache_.at(hash).original_size; ++stats_.l2_hits;}
  }
  auto &packed = StateStorePacked::instance();
  if (!compressed) {
    std::vector<uint8_t> bytes;
    if (packed.load_compressed(hash, bytes) < 0) return {};
    original_size = packed.get_original_size(hash);
    compressed = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    std::lock_guard lock(mutex_); check_ready(); ++stats_.disk_reads;
    insert_cache(warm_cache_, warm_lru_, warm_memory_usage_, config_.warm_cache_size, hash, compressed, original_size);
  }
  auto bytes = std::make_shared<const std::vector<uint8_t>>(packed.decompress(*compressed, original_size));
  if (bytes->empty() || XXHash64::hash(bytes->data(), bytes->size()) != hash)
    throw std::runtime_error("snapshot content hash mismatch");
  {
    std::lock_guard lock(mutex_); check_ready();
    insert_cache(hot_cache_, hot_lru_, hot_memory_usage_, config_.hot_cache_size, hash, bytes, bytes->size());
  }
  return bytes;
}
std::shared_ptr<const std::vector<uint8_t>> StateStore::load_shared(hash_type hash) {
  ensure_initialized(); std::shared_lock lifecycle(lifecycle_mutex_);
  return load_buffer(hash);
}
ssize_t StateStore::load(hash_type hash, std::vector<uint8_t> &out_data) {
  auto bytes = load_shared(hash);
  if (!bytes) {out_data.clear(); return -1;}
  out_data = *bytes; return static_cast<ssize_t>(out_data.size());
}
bool StateStore::exists(hash_type hash) {
  ensure_initialized(); std::shared_lock lifecycle(lifecycle_mutex_);
  std::lock_guard lock(mutex_); check_ready();
  return pending_.contains(hash) || hot_cache_.contains(hash) || warm_cache_.contains(hash) || StateStorePacked::instance().exists(hash);
}

void StateStore::queue_push_back(hash_type hash) { std::lock_guard lock(mutex_); state_queue_.push_back(hash); }
void StateStore::queue_pop_front() { (void)queue_try_pop_front(); }
hash_type StateStore::queue_try_pop_front() {
  hash_type hash;
  { std::lock_guard lock(mutex_); if (state_queue_.empty()) return 0;
    hash = state_queue_.front(); state_queue_.pop_front(); }
  submit_prefetch_tasks(); return hash;
}
size_t StateStore::queue_size() const { std::lock_guard lock(mutex_); return state_queue_.size(); }
void StateStore::queue_clear() { std::lock_guard lock(mutex_); state_queue_.clear(); }
hash_type StateStore::queue_peek(size_t offset) const {
  std::lock_guard lock(mutex_); return offset < state_queue_.size() ? state_queue_[offset] : 0;
}
void StateStore::disable_prefetch() { std::lock_guard lock(mutex_); config_.prefetch_window = 0; }
void StateStore::enable_prefetch(int window) {
  if (window < 0) throw std::invalid_argument("negative prefetch window");
  std::lock_guard lock(mutex_); config_.prefetch_window = static_cast<size_t>(window);
}
void StateStore::submit_prefetch_tasks() {
  std::shared_lock lifecycle(lifecycle_mutex_);
  std::vector<hash_type> candidates;
  {
    std::lock_guard lock(mutex_);
    if (!initialized_ || stopping_ || config_.prefetch_threads == 0 || config_.warm_cache_size == 0) return;
    check_ready();
    auto count = std::min(config_.prefetch_window, state_queue_.size());
    candidates.assign(state_queue_.begin(), state_queue_.begin() + count);
  }
  std::vector<hash_type> hashes;
  for (auto hash : candidates) {
    if (SysStateStore::instance().exists(hash)) {
      std::vector<uint8_t> data;
      if (!SysStateStore::instance().load(hash, data)) throw std::runtime_error("prefetch metadata disappeared");
      sys_state state;
      std::istringstream stream(std::string(reinterpret_cast<const char *>(data.data()), data.size()));
      { cereal::BinaryInputArchive archive(stream); archive(state); }
      for (auto thread_hash : state.ts_hash) if (thread_hash) hashes.push_back(thread_hash);
    } else hashes.push_back(hash);
  }
  std::lock_guard lock(mutex_); check_ready();
  for (auto hash : hashes) {
    if (prefetch_inflight_.size() >= config_.max_inflight_prefetch) break;
    if (pending_.contains(hash) || hot_cache_.contains(hash) || warm_cache_.contains(hash) || prefetch_inflight_.contains(hash)) continue;
    if (!StateStorePacked::instance().exists(hash)) continue;
    prefetch_inflight_.insert(hash);
    try { prefetch_queue_.push_back(hash); }
    catch (...) {prefetch_inflight_.erase(hash); throw;}
    ++stats_.prefetch_issued;
  }
  prefetch_cv_.notify_all();
}
void StateStore::prefetch_worker() {
  for (;;) {
    hash_type hash;
    {
      std::unique_lock lock(mutex_);
      prefetch_cv_.wait(lock, [&] {return stopping_ || failure_ || !prefetch_queue_.empty();});
      if (stopping_ || failure_) return;
      hash = prefetch_queue_.front(); prefetch_queue_.pop_front();
    }
    try {
      std::vector<uint8_t> data;
      auto &packed = StateStorePacked::instance();
      if (packed.load_compressed(hash, data) < 0) throw std::runtime_error("prefetch state disappeared");
      auto size = packed.get_original_size(hash);
      auto bytes = std::make_shared<const std::vector<uint8_t>>(std::move(data));
      std::lock_guard lock(mutex_);
      insert_cache(warm_cache_, warm_lru_, warm_memory_usage_, config_.warm_cache_size, hash, bytes, size);
      prefetch_inflight_.erase(hash);
    } catch (...) {
      std::lock_guard lock(mutex_); prefetch_inflight_.erase(hash);
      record_failure(std::current_exception()); return;
    }
  }
}

bool StateStore::in_l1(hash_type hash) const { std::lock_guard lock(mutex_); return hot_cache_.contains(hash); }
bool StateStore::in_l2(hash_type hash) const { std::lock_guard lock(mutex_); return warm_cache_.contains(hash); }
bool StateStore::in_preflight(hash_type hash) const { std::lock_guard lock(mutex_); return prefetch_inflight_.contains(hash); }
StateStore::Stats StateStore::get_stats() const { std::lock_guard lock(mutex_); return stats_; }
void StateStore::reset_stats() { std::lock_guard lock(mutex_); stats_ = {}; }
size_t StateStore::get_l1_entry_count() const { std::lock_guard lock(mutex_); return hot_cache_.size(); }
size_t StateStore::get_l2_entry_count() const { std::lock_guard lock(mutex_); return warm_cache_.size(); }
size_t StateStore::get_io_queue_size() const { std::lock_guard lock(mutex_); return pending_.size(); }
size_t StateStore::get_prefetch_queue_size() const { std::lock_guard lock(mutex_); return prefetch_queue_.size(); }
size_t StateStore::get_l1_capacity() const { std::lock_guard lock(mutex_); return config_.hot_cache_size; }
size_t StateStore::get_l2_capacity() const { std::lock_guard lock(mutex_); return config_.warm_cache_size; }
void StateStore::print_stats() const {
  const auto stats = get_stats();
  detsim::ui::ui_printf("StateStore: loads=%lu L1=%lu L2=%lu disk=%lu hit=%.1f%% saves=%lu new=%lu dedup=%.1f%%\n",
      stats.load_requests, stats.l1_hits, stats.l2_hits, stats.disk_reads, stats.hit_rate(), stats.save_calls, stats.save_new, stats.dedup_rate());
  detsim::ui::ui_printf("  Cache bytes: L1=%zu/%zu L2=%zu/%zu; pending=%zu; prefetch=%lu\n",
      get_l1_usage(), get_l1_capacity(), get_l2_usage(), get_l2_capacity(), get_io_queue_size(), stats.prefetch_issued);
}
extern "C" {
void state_store_init(void) { StateStore::instance().init(); }
uint64_t state_store_save(const void *data, size_t len) { return StateStore::instance().save(data, len); }
ssize_t state_store_load(uint64_t hash, void *data, size_t max_len) {
  auto bytes = StateStore::instance().load_shared(hash);
  if (!bytes) return -1;
  if (!data || max_len < bytes->size()) throw std::invalid_argument("snapshot output buffer too small");
  memcpy(data, bytes->data(), bytes->size()); return static_cast<ssize_t>(bytes->size());
}
int state_store_exists(uint64_t hash) {return StateStore::instance().exists(hash) ? 1 : 0;}
}
