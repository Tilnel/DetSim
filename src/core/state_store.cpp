/*
 * state_store.cpp - Tiered cache with prefetch implementation
 */

#include "state_store.h"
#include "cereal/archives/binary.hpp"
#include "debug.h"
#include "state.h"
#include "state_store_packed.h"
#include "sysstate_store.h"
#include "utils.h"
#include "xxhash.h"
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <pthread.h>
#include <sstream>
#include <zstd.h>
#include <zstd_errors.h>

StateStorePacked &state_store_packed_instance()
{
  return StateStorePacked::instance();
}

static uint64_t compute_hash(const void *data, size_t len)
{
  return XXHash64::hash(data, len);
}

/* ==============================================================================
 * Constructor / Destructor
 * ==============================================================================
 */

StateStore::StateStore()
    : config_(), hot_memory_usage_(0), warm_memory_usage_(0), access_counter_(0)
{
}

StateStore::~StateStore() { shutdown(); }

StateStore &StateStore::instance()
{
  static StateStore instance;
  return instance;
}

void StateStore::init(const Config &config)
{
  if (initialized_.exchange(true))
  {
    LOG_WARN("StateStore already initialized");
    return;
  }

  config_ = config;

  // Initialize packed storage
  StateStorePacked::Config packed_config;
  packed_config.compression_level = config_.compression_level;
  packed_config.storage_path = config_.packed_storage_path;
  StateStorePacked::instance().init(packed_config);
  LOG_INFO("StateStore using packed storage: path=%s",
           config_.packed_storage_path.c_str());

// Check malloc tuning (glibc-specific)
#ifdef __linux__
  const char *arena_max = getenv("MALLOC_ARENA_MAX");
  const char *mmap_threshold = getenv("MALLOC_MMAP_THRESHOLD_");

  if (!arena_max)
  {
    LOG_WARN("MALLOC_ARENA_MAX not set. Consider setting to 4 to limit 64MB "
             "arenas.");
    LOG_WARN("  Run with: export MALLOC_ARENA_MAX=4");
  }

  if (!mmap_threshold)
  {
    LOG_WARN(
        "MALLOC_MMAP_THRESHOLD_ not set. 512KB vectors may create new arenas.");
    LOG_WARN("  Run with: export MALLOC_MMAP_THRESHOLD_=1073741824 (1GB)");
  }
#endif

  // Start persistence workers
  for (size_t i = 0; i < config_.io_threads; i++)
  {
    io_threads_.emplace_back(&StateStore::io_worker, this);
    char name[16];
    snprintf(name, sizeof(name), "io_worker_%zu", i);
    pthread_setname_np(io_threads_.back().native_handle(), name);
  }

  // Start prefetch workers
  for (size_t i = 0; i < config_.prefetch_threads; i++)
  {
    prefetch_threads_.emplace_back(&StateStore::prefetch_worker, this);
    char name[16];
    snprintf(name, sizeof(name), "prefetch_%zu", i);
    pthread_setname_np(prefetch_threads_.back().native_handle(), name);
  }

  eviction_thread_ = std::thread(&StateStore::eviction_worker, this);
  pthread_setname_np(eviction_thread_.native_handle(), "eviction");

  LOG_INFO("StateStore initialized: hot=%zuMB, warm=%zuMB, prefetch_window=%zu",
           config_.hot_cache_size / (1024 * 1024),
           config_.warm_cache_size / (1024 * 1024), config_.prefetch_window);
}

void StateStore::init()
{
  Config default_config;
  init(default_config);
}

void StateStore::disable_prefetch() { config_.prefetch_window = 0; }

void StateStore::enable_prefetch(int prefetch_window)
{
  config_.prefetch_window = prefetch_window;
}

void StateStore::shutdown()
{
  if (shutdown_.exchange(true))
    return;

  // Notify all condition variables while holding associated mutexes
  // to prevent missed wakeups
  {
    std::lock_guard<std::mutex> lock(io_mutex_);
    io_cv_.notify_all();
    io_queue_not_full_cv_.notify_all();
  }
  {
    std::lock_guard<std::mutex> lock(prefetch_mutex_);
    prefetch_cv_.notify_all();
  }
  {
    std::lock_guard<std::mutex> lock(inflight_mutex_);
    inflight_cv_.notify_all();
  }
  {
    std::lock_guard<std::mutex> lock(eviction_mutex_);
    eviction_cv_.notify_all();
  }

  // Join persistence threads
  for (auto &t : io_threads_)
  {
    if (t.joinable())
      t.join();
  }

  // Join eviction thread
  if (eviction_thread_.joinable())
    eviction_thread_.join();

  // Join prefetch threads
  for (auto &t : prefetch_threads_)
  {
    if (t.joinable())
      t.join();
  }

  // Clear all caches to free memory (prevents "still reachable" in valgrind)
  {
    std::lock_guard<std::recursive_mutex> lock(hot_mutex_);
    hot_cache_.clear();
    hot_lru_.clear();
    hot_lru_index_.clear();
    hot_memory_usage_ = 0;
  }

  {
    std::lock_guard<std::recursive_mutex> lock(warm_mutex_);
    warm_cache_.clear();
    warm_lru_.clear();
    warm_lru_index_.clear();
    warm_memory_usage_ = 0;
  }

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    state_queue_.clear();
  }

  {
    std::lock_guard<std::mutex> lock(inflight_mutex_);
    prefetch_inflight_.clear();
  }

  {
    std::lock_guard<std::mutex> lock(prefetch_mutex_);
    while (!prefetch_queue_.empty())
    {
      prefetch_queue_.pop();
    }
  }

  // Flush packed storage index and segment info to ensure data consistency
  StateStorePacked::instance().save_segments_info();
  StateStorePacked::instance().flush_index();
  LOG_INFO(
      "StateStore shutdown: flushed packed storage index and segments info");

  // Clear IO queue (should already be empty after workers join)
  {
    std::lock_guard<std::mutex> lock(io_mutex_);
    while (!io_queue_.empty())
    {
      io_queue_.pop();
    }
  }
}

/* ==============================================================================
 * Queue Management (with Transparent Prefetch)
 * ==============================================================================
 */

void StateStore::queue_push_back(hash_type hash)
{
  std::lock_guard<std::mutex> lock(queue_mutex_);
  state_queue_.push_back(hash);
}

void StateStore::queue_pop_front()
{
  hash_type current_hash = 0;

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (state_queue_.empty())
      return;
    current_hash = state_queue_.front();
    state_queue_.pop_front();
  }

  // Trigger prefetch for upcoming states
  submit_prefetch_tasks();
}

hash_type StateStore::queue_try_pop_front()
{
  hash_type hash = 0;

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (state_queue_.empty())
    {
      LOG_DEBUG("queue_try_pop_front: queue empty");
      return 0;
    }
    hash = state_queue_.front();
    state_queue_.pop_front();
    if (state_queue_.size() % 100 == 0)
    {
      LOG_DEBUG("queue_try_pop_front: popped hash %016lx, remaining=%zu", hash,
                state_queue_.size());
    }
  }

  // Trigger prefetch for upcoming states
  submit_prefetch_tasks();

  return hash;
}

size_t StateStore::queue_size() const
{
  std::lock_guard<std::mutex> lock(queue_mutex_);
  return state_queue_.size();
}

void StateStore::queue_clear()
{
  std::lock_guard<std::mutex> lock(queue_mutex_);
  state_queue_.clear();
}

hash_type StateStore::queue_peek(size_t offset) const
{
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (offset >= state_queue_.size())
  {
    return 0;
  }
  return state_queue_[offset];
}

/* ==============================================================================
 * Prefetch System
 * ==============================================================================
 */

void StateStore::submit_prefetch_tasks()
{
  std::vector<hash_type> to_prefetch;
  size_t skipped_in_cache = 0;
  size_t skipped_not_on_disk = 0;
  size_t checked = 0;

  {
    std::lock_guard<std::mutex> inflight_lock(inflight_mutex_);

    if (prefetch_inflight_.size() >= config_.max_inflight_prefetch)
      return;

    for (size_t i = 0; i < config_.prefetch_window; i++)
    {
      hash_type h = queue_peek(i);
      if (h == 0)
        break;

      checked++;

      bool is_sys_state = SysStateStore::instance().exists(h);

      if (is_sys_state)
      {
        std::vector<uint8_t> sys_data;
        if (SysStateStore::instance().load(h, sys_data))
        {
          try
          {
            sys_state s;
            std::istringstream iss(
                std::string((char *)sys_data.data(), sys_data.size()));
            cereal::BinaryInputArchive inputArchive(iss);
            inputArchive(s);

            for (int j = 0; j < NP; j++)
            {
              hash_type ts_hash = s.ts_hash[j];
              if (ts_hash != 0 && !in_l1(ts_hash) && !in_l2(ts_hash) &&
                  prefetch_inflight_.find(ts_hash) == prefetch_inflight_.end())
              {
                if (StateStorePacked::instance().exists(ts_hash))
                {
                  prefetch_inflight_.insert(ts_hash);
                  to_prefetch.push_back(ts_hash);

                  if (prefetch_inflight_.size() >=
                      config_.max_inflight_prefetch)
                    break;
                }
              }
            }
          }
          catch (...)
          {
            LOG_WARN("Prefetch: failed to deserialize sys_state %016lx", h);
          }
        }
        continue;
      }

      if (!in_l1(h) && !in_l2(h) &&
          prefetch_inflight_.find(h) == prefetch_inflight_.end())
      {
        if (!StateStorePacked::instance().exists(h))
        {
          skipped_not_on_disk++;
          continue;
        }

        prefetch_inflight_.insert(h);
        to_prefetch.push_back(h);

        if (prefetch_inflight_.size() >= config_.max_inflight_prefetch)
          break;
      }
      else
      {
        skipped_in_cache++;
      }
    }
  }

  if (!to_prefetch.empty())
  {
    LOG_DEBUG("Prefetch: submitting %zu tasks (%zu checked, %zu skipped cache, "
              "%zu not on disk)",
              to_prefetch.size(), checked, skipped_in_cache,
              skipped_not_on_disk);
  }

  // Submit tasks
  {
    std::lock_guard<std::mutex> lock(prefetch_mutex_);
    for (auto h : to_prefetch)
    {
      prefetch_queue_.push({h, std::chrono::steady_clock::now()});
    }
    prefetch_cv_.notify_all();
  }

  // Update stats
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.prefetch_issued += to_prefetch.size();
  }
}

void StateStore::prefetch_worker()
{
  while (!shutdown_.load())
  {
    PrefetchTask task;

    {
      std::unique_lock<std::mutex> lock(prefetch_mutex_);
      prefetch_cv_.wait(
          lock,
          [this] { return !prefetch_queue_.empty() || shutdown_.load(); });

      if (shutdown_.load())
        break;
      if (prefetch_queue_.empty())
        continue;

      task = prefetch_queue_.front();
      prefetch_queue_.pop();
    }

    // Check if already loaded by main thread
    if (in_l1(task.hash) || in_l2(task.hash))
    {
      std::lock_guard<std::mutex> lock(inflight_mutex_);
      prefetch_inflight_.erase(task.hash);
      inflight_cv_.notify_all();
      continue;
    }

    // Try to load from disk to L2
    std::vector<uint8_t> compressed;
    if (read_compressed_from_disk(task.hash, compressed))
    {
      insert_to_l2(task.hash, std::move(compressed));
      LOG_TRACE("Prefetched hash %016lx to L2", task.hash);
    }
    // Silently skip if not found (could be sys_state stored as .ss, or race
    // condition)

    // Remove from inflight
    {
      std::lock_guard<std::mutex> lock(inflight_mutex_);
      prefetch_inflight_.erase(task.hash);
      inflight_cv_.notify_all();
    }
  }
}

bool StateStore::in_l1(hash_type hash) const
{
  std::lock_guard<std::recursive_mutex> lock(hot_mutex_);
  auto it = hot_cache_.find(hash);
  return it != hot_cache_.end() && !it->second->raw_data.empty();
}

bool StateStore::in_l2(hash_type hash) const
{
  std::lock_guard<std::recursive_mutex> lock(warm_mutex_);
  auto it = warm_cache_.find(hash);
  return it != warm_cache_.end() && !it->second->compressed_data.empty();
}

bool StateStore::in_preflight(hash_type hash) const
{
  std::lock_guard<std::mutex> lock(inflight_mutex_);
  return prefetch_inflight_.find(hash) != prefetch_inflight_.end();
}

/* ==============================================================================
 * Cache Operations
 * ==============================================================================
 */

void StateStore::insert_to_l1(hash_type hash, std::vector<uint8_t> &&raw_data)
{
  if (raw_data.empty())
    return;

  // Optimization A: Allow 20% overflow before triggering eviction
  // Reduces eviction frequency during bursts
  const double overflow_ratio = 1.2;
  size_t eviction_threshold =
      static_cast<size_t>(config_.hot_cache_size * overflow_ratio);

  // Optimization C: Batch eviction - evict at least 10% of capacity each time
  const size_t min_eviction_batch = config_.hot_cache_size / 10;

  if (hot_memory_usage_.load() + raw_data.size() > eviction_threshold)
  {
    size_t to_evict =
        hot_memory_usage_.load() + raw_data.size() - config_.hot_cache_size;
    to_evict = std::max(to_evict, min_eviction_batch);
    evict_l1_async(to_evict);
  }

  std::lock_guard<std::recursive_mutex> lock(hot_mutex_);

  // Use try_emplace to avoid double lookup for new entries
  auto [it, inserted] = hot_cache_.try_emplace(hash, nullptr);

  if (!inserted)
  {
    // Entry exists - update it
    hot_memory_usage_ -= it->second->raw_data.size();
    it->second->raw_data = std::move(raw_data);
    it->second->init_shared_ptr();
    it->second->state = StateEntry::State::RAW;
    it->second->last_access = ++access_counter_;

    auto idx_it = hot_lru_index_.find(hash);
    if (idx_it != hot_lru_index_.end())
    {
      hot_lru_.erase(idx_it->second);
    }
    hot_lru_.push_front(hash);
    hot_lru_index_[hash] = hot_lru_.begin();

    hot_memory_usage_ += it->second->raw_data.size();
    return;
  }

  // New entry - construct it
  auto entry = std::make_shared<StateEntry>(hash);
  entry->raw_data = std::move(raw_data);
  entry->init_shared_ptr();
  entry->state = StateEntry::State::RAW;
  entry->last_access = ++access_counter_;

  it->second = entry;
  hot_lru_.push_front(hash);
  hot_lru_index_[hash] = hot_lru_.begin();

  hot_memory_usage_ += entry->raw_data.size();
}

void StateStore::insert_to_l2(hash_type hash,
                              std::vector<uint8_t> &&compressed_data)
{
  if (compressed_data.empty())
    return;

  evict_l2_if_needed(compressed_data.size());

  std::lock_guard<std::recursive_mutex> lock(warm_mutex_);

  // Check if already exists
  auto it = warm_cache_.find(hash);
  if (it != warm_cache_.end())
  {
    // Update existing entry
    warm_memory_usage_ -= it->second->compressed_data.size();
    it->second->compressed_data = std::move(compressed_data);
    it->second->state = StateEntry::State::COMPRESSED;
    it->second->last_access = ++access_counter_;
    warm_memory_usage_ += it->second->compressed_data.size();

    // Update LRU
    warm_lru_.erase(warm_lru_index_[hash]);
    warm_lru_.push_front(hash);
    warm_lru_index_[hash] = warm_lru_.begin();
    return;
  }

  auto entry = std::make_shared<StateEntry>(hash);
  entry->compressed_data = std::move(compressed_data);
  entry->state = StateEntry::State::COMPRESSED;
  entry->last_access = ++access_counter_;

  warm_cache_[hash] = entry;
  warm_lru_.push_front(hash);
  warm_lru_index_[hash] = warm_lru_.begin();

  warm_memory_usage_ += entry->compressed_data.size();
}

bool StateStore::move_l2_to_l1(hash_type hash)
{
  StateEntryPtr entry;
  std::vector<uint8_t> compressed_data;

  // Phase 1: Extract data from L2 (warm_mutex_ only)
  {
    std::lock_guard<std::recursive_mutex> lock(warm_mutex_);
    auto it = warm_cache_.find(hash);
    if (it == warm_cache_.end() || it->second->compressed_data.empty())
    {
      return false;
    }
    entry = it->second;
    // Copy compressed data while holding lock
    compressed_data = entry->compressed_data;
  }

  // Phase 2: Decompress outside any lock
  size_t original_size = StateStorePacked::instance().get_original_size(hash);
  std::vector<uint8_t> raw = decompress(compressed_data, original_size);
  if (raw.empty())
  {
    LOG_ERROR("Failed to decompress hash %016lx for L1 promotion", hash);
    return false;
  }

  // Phase 3: Insert to L1 (hot_mutex_ only, NO nested warm_mutex_)
  bool already_in_l1 = false;
  {
    std::lock_guard<std::recursive_mutex> hot_lock(hot_mutex_);

    // Check if already in L1
    if (hot_cache_.find(hash) != hot_cache_.end())
    {
      already_in_l1 = true;
    }
    else
    {
      // Evict from L1 if needed
      size_t required_bytes = raw.size();
      while (hot_memory_usage_ + required_bytes > config_.hot_cache_size &&
             !hot_lru_.empty())
      {
        hash_type oldest = hot_lru_.back();
        auto it = hot_cache_.find(oldest);
        if (it != hot_cache_.end())
        {
          size_t freed = it->second->raw_data.size();
          std::vector<uint8_t>().swap(it->second->raw_data);
          hot_memory_usage_ -= freed;

          if (it->second->compressed_data.empty())
            it->second->state = StateEntry::State::PERSISTED;
          else
            it->second->state = StateEntry::State::COMPRESSED;

          hot_cache_.erase(it);
        }
        hot_lru_.pop_back();
        hot_lru_index_.erase(oldest);
      }

      // Create new entry and insert to L1
      auto new_entry = std::make_shared<StateEntry>(hash);
      new_entry->raw_data = std::move(raw);
      new_entry->init_shared_ptr();
      new_entry->state = StateEntry::State::RAW;
      new_entry->last_access = ++access_counter_;

      hot_cache_[hash] = new_entry;
      hot_lru_.push_front(hash);
      hot_lru_index_[hash] = hot_lru_.begin();
      hot_memory_usage_ += new_entry->raw_data.size();
    }
  } // Release hot_mutex_ before acquiring warm_mutex_

  // Phase 4: Update L2 state (warm_mutex_ only, NO nested locks)
  // This avoids AB-BA deadlock by never holding both locks simultaneously
  {
    std::lock_guard<std::recursive_mutex> warm_lock(warm_mutex_);
    entry->state = StateEntry::State::RAW; // Now also in L1
  }

  return true;
}

void StateStore::evict_l2_if_needed(size_t required_bytes)
{
  size_t total_evicted = 0;
  bool should_trim = false;

  {
    std::lock_guard<std::recursive_mutex> lock(warm_mutex_);

    while (warm_memory_usage_ + required_bytes > config_.warm_cache_size &&
           !warm_lru_.empty())
    {
      hash_type oldest = warm_lru_.back();
      auto it = warm_cache_.find(oldest);

      if (it != warm_cache_.end())
      {
        size_t freed = it->second->compressed_data.size();
        warm_memory_usage_ -= freed;
        total_evicted += freed;
        warm_cache_.erase(it);
      }

      warm_lru_.pop_back();
      warm_lru_index_.erase(oldest);
    }

// Check if we should trim memory (outside lock)
#ifdef __linux__
    if (config_.enable_malloc_trim && total_evicted > 0)
    {
      static std::atomic<size_t> accumulated_evicted{0};
      size_t new_total = accumulated_evicted += total_evicted;

      if (new_total >= config_.malloc_trim_threshold)
      {
        accumulated_evicted = 0;
        should_trim = true;
      }
    }
#endif
  } // unlock here

// Release memory back to OS (outside lock to avoid blocking)
#ifdef __linux__
  if (should_trim)
  {
    malloc_trim(0);
  }
#endif
}

void StateStore::update_hot_lru(hash_type hash)
{
  std::lock_guard<std::recursive_mutex> lock(hot_mutex_);
  auto it = hot_lru_index_.find(hash);
  if (it != hot_lru_index_.end())
  {
    hot_lru_.erase(it->second);
    hot_lru_.push_front(hash);
    it->second = hot_lru_.begin();
  }
}

void StateStore::update_warm_lru(hash_type hash)
{
  std::lock_guard<std::recursive_mutex> lock(warm_mutex_);
  auto it = warm_lru_index_.find(hash);
  if (it != warm_lru_index_.end())
  {
    warm_lru_.erase(it->second);
    warm_lru_.push_front(hash);
    it->second = warm_lru_.begin();
  }
}

void StateStore::evict_l1_async(size_t target_eviction)
{
  auto start_time = std::chrono::high_resolution_clock::now();

  std::lock_guard<std::recursive_mutex> lock(hot_mutex_);

  size_t evicted = 0;
  size_t attempts = 0;
  size_t max_attempts = hot_lru_.size() * 2;

  while (evicted < target_eviction && !hot_lru_.empty() &&
         attempts < max_attempts)
  {
    hash_type oldest = hot_lru_.back();
    auto it = hot_cache_.find(oldest);
    attempts++;

    if (it != hot_cache_.end())
    {
      StateEntryPtr entry = it->second;
      StateEntry::State current_state = entry->state.load();

      // For RAW entries, compress inline (CPU only, no I/O blocking)
      // Then move to L2 and evict immediately. io_worker will persist async.
      if (current_state == StateEntry::State::RAW)
      {
        std::vector<uint8_t> compressed = compress(entry->raw_data);
        if (!compressed.empty())
        {
          // Move compressed data to L2 immediately
          insert_to_l2(entry->hash, std::move(compressed));

          // Mark as COMPRESSED (raw_data will be freed during eviction below)
          {
            std::lock_guard<std::mutex> entry_lock(entry->mutex);
            entry->state = StateEntry::State::COMPRESSED;
          }

          // Submit to io_queue for async disk persistence
          // io_worker will write L2 data to disk without blocking us
          {
            std::lock_guard<std::mutex> io_lock(io_mutex_);
            if (io_queue_.size() < max_io_queue_size_)
            {
              io_queue_.push(entry);
              io_cv_.notify_one();
            }
          }

          current_state = StateEntry::State::COMPRESSED;
        }
      }

      // Only evict PERSISTED or COMPRESSED entries
      if (current_state != StateEntry::State::PERSISTED &&
          current_state != StateEntry::State::COMPRESSED)
      {
        hot_lru_.pop_back();
        hot_lru_.push_front(oldest);
        hot_lru_index_[oldest] = hot_lru_.begin();
        if (hot_lru_.size() == 1)
          break;
        continue;
      }

      size_t to_evict = entry->raw_data.size();
      evicted += to_evict;
      hot_memory_usage_ -= to_evict;

      // Free raw_data immediately (compression result is in L2)
      {
        std::lock_guard<std::mutex> entry_lock(entry->mutex);
        std::vector<uint8_t>().swap(entry->raw_data);
      }

      hot_cache_.erase(it);

      {
        std::lock_guard<std::mutex> evict_lock(eviction_mutex_);
        eviction_queue_.push(entry);
        eviction_cv_.notify_one();
      }
    }

    hot_lru_.pop_back();
    hot_lru_index_.erase(oldest);
  }

  auto end_time = std::chrono::high_resolution_clock::now();
  auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        end_time - start_time)
                        .count();
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.evict_calls++;
    stats_.evict_bytes += evicted;
    stats_.evict_time_us += elapsed_us;
  }
}

void StateStore::eviction_worker()
{
  while (!shutdown_.load())
  {
    StateEntryPtr entry;
    {
      std::unique_lock<std::mutex> lock(eviction_mutex_);
      eviction_cv_.wait(
          lock,
          [this] { return !eviction_queue_.empty() || shutdown_.load(); });

      if (shutdown_.load())
        break;
      if (eviction_queue_.empty())
        continue;

      entry = eviction_queue_.front();
      eviction_queue_.pop();
    }

    if (!entry || entry->raw_data.empty())
      continue;

    // Use already compressed data from io_worker (avoid double compression)
    std::vector<uint8_t> compressed;
    {
      std::lock_guard<std::mutex> entry_lock(entry->mutex);
      compressed = entry->compressed_data;
    }

    if (!compressed.empty())
    {
      insert_to_l2(entry->hash, std::move(compressed));

      {
        std::lock_guard<std::mutex> entry_lock(entry->mutex);
        std::vector<uint8_t>().swap(entry->raw_data);
        entry->state = StateEntry::State::COMPRESSED;
      }
    }
  }
}

/* ==============================================================================
 * Save / Load
 * ==============================================================================
 */

hash_type StateStore::save(const void *data, size_t len)
{
  if (!data || len == 0)
    return 0;

  hash_type hash = compute_hash(data, len);

  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.save_calls++;
  }

  // Check if already exists (single lock scope to avoid deadlock)
  bool in_hot = false, in_warm = false;
  {
    std::lock_guard<std::recursive_mutex> lock(hot_mutex_);
    auto it = hot_cache_.find(hash);
    in_hot = (it != hot_cache_.end() && !it->second->raw_data.empty());
    if (in_hot)
    {
      // Update LRU while holding lock
      auto idx_it = hot_lru_index_.find(hash);
      if (idx_it != hot_lru_index_.end())
      {
        hot_lru_.erase(idx_it->second);
        hot_lru_.push_front(hash);
        idx_it->second = hot_lru_.begin();
      }
    }
  }

  if (in_hot)
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.save_dedup_hot++;
    return hash;
  }

  if (in_l2(hash))
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.save_dedup_warm++;
    update_warm_lru(hash);
    return hash;
  }

  if (exists_on_disk(hash))
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.save_dedup_disk++;
    LOG_DEBUG("StateStore::save: hash %016lx already exists on disk, skipping",
              hash);
    return hash;
  }

  auto save_start = std::chrono::high_resolution_clock::now();

  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.save_new++;
  }

  // Insert to L1 (raw) - NO COMPRESSION HERE
  std::vector<uint8_t> raw(len);
  memcpy(raw.data(), data, len);
  insert_to_l1(hash, std::move(raw));

  // Get entry for async processing (compression + persistence)
  StateEntryPtr entry;
  {
    std::lock_guard<std::recursive_mutex> lock(hot_mutex_);
    entry = hot_cache_[hash];
  }

  // NOTE: Compression is now done asynchronously in io_worker
  // This reduces main thread latency significantly

  // Trigger async compression and disk write
  // Use backpressure: wait if queue is full to prevent OOM
  {
    std::unique_lock<std::mutex> lock(io_mutex_);
    // Wait until queue has space (backpressure)
    io_queue_not_full_cv_.wait(
        lock, [this]
        { return io_queue_.size() < max_io_queue_size_ || shutdown_.load(); });
    if (shutdown_.load())
      return hash;
    io_queue_.push(entry);
  }
  io_cv_.notify_one();

  auto save_end = std::chrono::high_resolution_clock::now();
  auto save_elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                             save_end - save_start)
                             .count();

  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.save_time_us += save_elapsed_us;
    if (save_elapsed_us > 1000)
    {
      stats_.save_new_slow++;
    }
  }

  LOG_TRACE("StateStore saved hash %016lx: raw=%zu (async compression)", hash,
            len);
  return hash;
}

ssize_t StateStore::load(hash_type hash, std::vector<uint8_t> &out_data)
{
  if (hash == 0)
    return -1;

  // Update statistics
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.load_requests++;
  }

  // Try L1 first
  {
    std::lock_guard<std::recursive_mutex> lock(hot_mutex_);
    auto it = hot_cache_.find(hash);
    if (it != hot_cache_.end() && !it->second->raw_data.empty())
    {
      out_data = it->second->raw_data;
      it->second->last_access = ++access_counter_;
      // Update LRU inline (already holding lock)
      auto idx_it = hot_lru_index_.find(hash);
      if (idx_it != hot_lru_index_.end())
      {
        hot_lru_.erase(idx_it->second);
        hot_lru_.push_front(hash);
        idx_it->second = hot_lru_.begin();
      }
      // Update stats
      {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.l1_hits++;
      }
      LOG_TRACE("StateStore load %016lx from L1", hash);
      return static_cast<ssize_t>(out_data.size());
    }
  }

  // Try L2 (decompress to L1)
  std::vector<uint8_t> compressed_from_l2;
  {
    std::lock_guard<std::recursive_mutex> lock(warm_mutex_);
    auto it = warm_cache_.find(hash);
    if (it != warm_cache_.end() && !it->second->compressed_data.empty())
    {
      // Copy compressed data before releasing lock
      compressed_from_l2 = it->second->compressed_data;
      it->second->last_access = ++access_counter_;
      // Update LRU inline (already holding lock)
      auto idx_it = warm_lru_index_.find(hash);
      if (idx_it != warm_lru_index_.end())
      {
        warm_lru_.erase(idx_it->second);
        warm_lru_.push_front(hash);
        idx_it->second = warm_lru_.begin();
      }
    }
  }

  // Decompress outside the lock
  if (!compressed_from_l2.empty())
  {
    size_t original_size = StateStorePacked::instance().get_original_size(hash);
    std::vector<uint8_t> decompressed =
        decompress(compressed_from_l2, original_size);
    if (decompressed.empty())
    {
      LOG_ERROR("Decompression failed for %016lx", hash);
      return -1;
    }

    // Always promote to L1 - allow temporary overflow, eviction worker will
    // clean up
    insert_to_l1(hash, std::move(decompressed));
    // Reference L1 data for return (avoid another copy)
    {
      std::lock_guard<std::recursive_mutex> lock(hot_mutex_);
      auto it = hot_cache_.find(hash);
      if (it != hot_cache_.end() && !it->second->raw_data.empty())
      {
        out_data = it->second->raw_data;
      }
      else
      {
        out_data = std::move(decompressed);
      }
    }

    // Update stats
    {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      stats_.l2_hits++;
    }
    LOG_TRACE("StateStore load %016lx from L2", hash);
    return static_cast<ssize_t>(out_data.size());
  }

  // Try disk - load compressed data directly to avoid re-compression
  if (exists_on_disk(hash))
  {
    std::vector<uint8_t> compressed_data;
    if (read_compressed_from_disk(hash, compressed_data))
    {
      // Get original size before moving compressed_data
      size_t original_size =
          StateStorePacked::instance().get_original_size(hash);

      // Insert to L2 first (already compressed)
      insert_to_l2(hash, std::move(compressed_data));

      // Get the compressed data from L2 and decompress for L1
      std::vector<uint8_t> decompressed;
      {
        std::lock_guard<std::recursive_mutex> lock(warm_mutex_);
        auto it = warm_cache_.find(hash);
        if (it != warm_cache_.end() && !it->second->compressed_data.empty())
        {
          decompressed = decompress(it->second->compressed_data, original_size);
        }
      }

      if (!decompressed.empty())
      {
        insert_to_l1(hash, std::move(decompressed));

        // Reference L1 data for return
        {
          std::lock_guard<std::recursive_mutex> lock(hot_mutex_);
          auto it = hot_cache_.find(hash);
          if (it != hot_cache_.end() && !it->second->raw_data.empty())
          {
            out_data = it->second->raw_data;
          }
          else
          {
            out_data = std::move(decompressed);
          }
        }

        // Update stats
        {
          std::lock_guard<std::mutex> lock(stats_mutex_);
          stats_.disk_reads++;
        }
        LOG_TRACE("StateStore load %016lx from disk", hash);
        return static_cast<ssize_t>(out_data.size());
      }
    }
    LOG_ERROR("Disk read failed for hash %016lx", hash);
  }
  else
  {
    LOG_ERROR("Hash %016lx not found in L1, L2, or disk", hash);
  }

  return -1;
}

std::shared_ptr<const std::vector<uint8_t>>
StateStore::load_shared(hash_type hash)
{
  if (hash == 0)
    return nullptr;

  {
    std::lock_guard<std::recursive_mutex> lock(hot_mutex_);
    auto it = hot_cache_.find(hash);
    if (it != hot_cache_.end() && it->second->raw_data_ptr)
    {
      it->second->last_access = ++access_counter_;
      auto idx_it = hot_lru_index_.find(hash);
      if (idx_it != hot_lru_index_.end())
      {
        hot_lru_.erase(idx_it->second);
        hot_lru_.push_front(hash);
        idx_it->second = hot_lru_.begin();
      }
      {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.load_requests++;
        stats_.l1_hits++;
      }
      LOG_TRACE("StateStore load_shared %016lx from L1", hash);
      return it->second->raw_data_ptr;
    }
  }

  std::vector<uint8_t> data;
  if (load(hash, data) > 0)
  {
    std::lock_guard<std::recursive_mutex> lock(hot_mutex_);
    auto it = hot_cache_.find(hash);
    if (it != hot_cache_.end() && it->second->raw_data_ptr)
    {
      return it->second->raw_data_ptr;
    }
    // L2/disk path didn't insert to L1 (maybe L1 full), create shared_ptr from
    // returned data
    return std::make_shared<std::vector<uint8_t>>(std::move(data));
  }
  return nullptr;
}

bool StateStore::exists(hash_type hash)
{
  if (in_l1(hash) || in_l2(hash))
    return true;
  return exists_on_disk(hash);
}

void StateStore::wait_for_completion()
{
  // Wait for all pending IO operations to complete
  // This is used when SIGINT is received to ensure data consistency
  std::unique_lock<std::mutex> lock(io_mutex_);

  // Wait until IO queue is empty
  while (!io_queue_.empty())
  {
    lock.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    lock.lock();
  }

  // Also wait for any in-flight writes (state == PERSISTING)
  // by checking all entries in hot cache
  lock.unlock();

  std::vector<StateEntryPtr> pending_entries;
  {
    std::lock_guard<std::recursive_mutex> hot_lock(hot_mutex_);
    for (auto &[hash, entry] : hot_cache_)
    {
      if (entry->state.load() == StateEntry::State::PERSISTING)
      {
        pending_entries.push_back(entry);
      }
    }
  }

  // Wait for each pending entry
  for (auto &entry : pending_entries)
  {
    std::unique_lock<std::mutex> entry_lock(entry->mutex);
    entry->cv.wait(entry_lock,
                   [&entry]() {
                     return entry->state.load() == StateEntry::State::PERSISTED;
                   });
  }

  // Flush packed storage index to ensure index entries are persisted
  StateStorePacked::instance().save_segments_info();
  StateStorePacked::instance().flush_index();
  LOG_INFO("StateStore wait_for_completion: flushed packed storage index");
}

/* ==============================================================================
 * Background I/O Worker (Persistence)
 * ==============================================================================
 */

void StateStore::io_worker()
{
  while (!shutdown_.load())
  {
    StateEntryPtr entry;

    {
      std::unique_lock<std::mutex> lock(io_mutex_);
      io_cv_.wait(lock,
                  [this] { return !io_queue_.empty() || shutdown_.load(); });

      if (shutdown_.load())
        break;
      if (io_queue_.empty())
        continue;

      entry = io_queue_.front();
      io_queue_.pop();
      // Notify producers that queue has space
      io_queue_not_full_cv_.notify_one();
    }

    if (!entry || entry->raw_data.empty())
      continue;

    // Lock and copy raw_data to prevent use-after-free during compression
    // (eviction may clear raw_data from another thread)
    std::vector<uint8_t> raw_data;
    {
      std::lock_guard<std::mutex> entry_lock(entry->mutex);
      raw_data = entry->raw_data;
    }
    const size_t original_size = raw_data.size();

    // Async compression
    std::vector<uint8_t> compressed = compress(raw_data);

    if (!compressed.empty())
    {
      // Store compressed in entry
      {
        std::lock_guard<std::mutex> entry_lock(entry->mutex);
        entry->compressed_data = std::move(compressed);
        entry->state = StateEntry::State::COMPRESSED;
      }

      // Note: L2 cache is filled during load, not here (to avoid deadlocks)
      // Mark as persisting and write to disk
      {
        std::lock_guard<std::mutex> entry_lock(entry->mutex);
        entry->state = StateEntry::State::PERSISTING;
      }

      bool success =
          write_to_disk(entry->hash, entry->compressed_data, original_size);

      if (success)
      {
        // Keep data in L1 - only mark as PERSISTED and store compressed in
        // entry Actual eviction to L2 happens via evict_l1_async when L1 is
        // full
        {
          std::lock_guard<std::mutex> entry_lock(entry->mutex);
          entry->state = StateEntry::State::PERSISTED;
        }

        // Insert compressed data to L2 for redundancy (but keep in L1 too)
        std::vector<uint8_t> compressed_for_l2;
        {
          std::lock_guard<std::mutex> entry_lock(entry->mutex);
          compressed_for_l2 = entry->compressed_data;
        }
        insert_to_l2(entry->hash, std::move(compressed_for_l2));

        LOG_TRACE(
            "Persisted hash %016lx (async), kept in L1, also stored in L2",
            entry->hash);
      }
      else
      {
        LOG_ERROR("Failed to persist hash %016lx", entry->hash);
      }

      {
        std::lock_guard<std::mutex> entry_lock(entry->mutex);
        entry->cv.notify_all();
      }
    }
  }
}

/* ==============================================================================
 * Compression - delegated to StateStorePacked
 * ==============================================================================
 */

std::vector<uint8_t> StateStore::compress(const std::vector<uint8_t> &data)
{
  return StateStorePacked::instance().compress(data);
}

std::vector<uint8_t> StateStore::decompress(const std::vector<uint8_t> &data,
                                            size_t original_size)
{
  return StateStorePacked::instance().decompress(data, original_size);
}

/* ==============================================================================
 * Disk Operations
 * ==============================================================================
 */

bool StateStore::write_to_disk(hash_type hash,
                               const std::vector<uint8_t> &compressed_data,
                               size_t original_size)
{
  StateStorePacked &packed = StateStorePacked::instance();
  hash_type result = packed.save(compressed_data.data(), compressed_data.size(),
                                 hash, true, original_size);
  return result != 0;
}

bool StateStore::read_from_disk(hash_type hash, std::vector<uint8_t> &data)
{
  StateStorePacked &packed = StateStorePacked::instance();
  ssize_t result = packed.load(hash, data);
  return result > 0;
}

bool StateStore::read_compressed_from_disk(
    hash_type hash, std::vector<uint8_t> &compressed_data)
{
  StateStorePacked &packed = StateStorePacked::instance();
  return packed.load_compressed(hash, compressed_data) > 0;
}

bool StateStore::exists_on_disk(hash_type hash)
{
  return StateStorePacked::instance().exists(hash);
}

/* ==============================================================================
 * Statistics
 * ==============================================================================
 */

StateStore::Stats StateStore::get_stats() const
{
  std::lock_guard<std::mutex> lock(stats_mutex_);
  return stats_;
}

void StateStore::reset_stats()
{
  std::lock_guard<std::mutex> lock(stats_mutex_);
  stats_ = Stats();
}

size_t StateStore::get_l1_entry_count() const
{
  std::lock_guard<std::recursive_mutex> lock(hot_mutex_);
  return hot_cache_.size();
}

size_t StateStore::get_l2_entry_count() const
{
  std::lock_guard<std::recursive_mutex> lock(
      const_cast<std::recursive_mutex &>(warm_mutex_));
  return warm_cache_.size();
}

size_t StateStore::get_io_queue_size() const
{
  std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(io_mutex_));
  return io_queue_.size();
}

size_t StateStore::get_prefetch_queue_size() const
{
  std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(prefetch_mutex_));
  return prefetch_queue_.size();
}

void StateStore::print_stats() const
{
  Stats stats = get_stats();

  detsim::ui::ui_printf("\n=== StateStore Cache Statistics ===\n");
  detsim::ui::ui_printf("Load requests:     %lu\n", stats.load_requests);
  detsim::ui::ui_printf("  L1 hits:         %lu (%.1f%%)\n", stats.l1_hits,
                        stats.load_requests > 0
                            ? 100.0 * stats.l1_hits / stats.load_requests
                            : 0.0);
  detsim::ui::ui_printf("  L2 hits:         %lu (%.1f%%)\n", stats.l2_hits,
                        stats.load_requests > 0
                            ? 100.0 * stats.l2_hits / stats.load_requests
                            : 0.0);
  detsim::ui::ui_printf("  Disk reads:      %lu (%.1f%%)\n", stats.disk_reads,
                        stats.load_requests > 0
                            ? 100.0 * stats.disk_reads / stats.load_requests
                            : 0.0);
  detsim::ui::ui_printf("  Hit rate:        %.1f%%\n", stats.hit_rate());

  size_t l1_usage = hot_memory_usage_.load();
  size_t l2_usage = warm_memory_usage_.load();
  detsim::ui::ui_printf("\nCache Usage:\n");
  detsim::ui::ui_printf("  L1 (Hot):        %zu MB / %zu MB (%.1f%%)\n",
                        l1_usage / (1024 * 1024),
                        config_.hot_cache_size / (1024 * 1024),
                        100.0 * l1_usage / config_.hot_cache_size);
  detsim::ui::ui_printf("  L2 (Warm):       %zu MB / %zu MB (%.1f%%)\n",
                        l2_usage / (1024 * 1024),
                        config_.warm_cache_size / (1024 * 1024),
                        100.0 * l2_usage / config_.warm_cache_size);
  detsim::ui::ui_printf("  Prefetch issued: %lu\n", stats.prefetch_issued);

  detsim::ui::ui_printf("\nEviction Statistics:\n");
  detsim::ui::ui_printf("  Evict calls:     %lu\n", stats.evict_calls);
  detsim::ui::ui_printf("  Bytes evicted:   %zu MB\n",
                        stats.evict_bytes / (1024 * 1024));
  detsim::ui::ui_printf("  Evict time:      %lu ms\n",
                        stats.evict_time_us / 1000);
  if (stats.evict_calls > 0)
  {
    detsim::ui::ui_printf("  Avg evict time:  %lu us/call\n",
                          stats.evict_time_us / stats.evict_calls);
  }

  detsim::ui::ui_printf("\nSave Path Timing:\n");
  detsim::ui::ui_printf("  New states saved: %lu\n", stats.save_new);
  detsim::ui::ui_printf("  Total save time:  %lu ms\n",
                        stats.save_time_us / 1000);
  if (stats.save_new > 0)
  {
    detsim::ui::ui_printf("  Avg save time:    %lu us/call\n",
                          stats.save_time_us / stats.save_new);
  }
  detsim::ui::ui_printf(
      "  Slow saves (>1ms): %lu (%.2f%%)\n", stats.save_new_slow,
      stats.save_new > 0 ? 100.0 * stats.save_new_slow / stats.save_new : 0.0);
  detsim::ui::ui_printf("====================================\n");
}

/* ==============================================================================
 * C Interface
 * ==============================================================================
 */

extern "C"
{

  void state_store_init(void) { StateStore::instance().init(); }

  uint32_t state_store_save(const void *data, size_t len)
  {
    return StateStore::instance().save(data, len);
  }

  ssize_t state_store_load(uint64_t hash, void *data, size_t max_len)
  {
    std::vector<uint8_t> buffer;
    ssize_t result = StateStore::instance().load(hash, buffer);
    if (result < 0)
      return -1;

    size_t copy_len = std::min(static_cast<size_t>(result), max_len);
    memcpy(data, buffer.data(), copy_len);
    return result;
  }

  int state_store_exists(uint64_t hash)
  {
    return StateStore::instance().exists(hash) ? 1 : 0;
  }

} // extern "C"
