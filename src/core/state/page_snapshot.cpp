#include "page_snapshot.h"
#include <algorithm>
#include <cstring>
#include <list>
#include <stdexcept>
#include <sys/mman.h>

namespace {
constexpr uint64_t page_size = 4096;
constexpr size_t cache_limit = 4 * 1024 * 1024;
void require(bool valid, const char* message) {
  if (!valid) throw std::runtime_error(message);
}
size_t chunk_length(uint64_t address, uint64_t end) {
  return std::min<uint64_t>(end - address, page_size - address % page_size);
}
struct CacheEntry {
  hash_type hash;
  std::shared_ptr<const SnapshotView> view;
  size_t bytes;
};
std::mutex cache_mutex;
std::list<CacheEntry> cache;
uint64_t cache_generation = 0;
size_t cache_bytes = 0;
}

// Both storage encodings expose the same immutable chunk reader. SnapshotView
// owns format validation, region lookup and lifetime checks above this boundary.
class SnapshotView::Memory {
 public:
  virtual ~Memory() = default;
  virtual OwnedSpan page(const RegionInfo& region, size_t index) const = 0;
};
namespace {
class FlatMemory final : public SnapshotView::Memory {
  std::shared_ptr<const std::vector<uint8_t>> data_;
 public:
  explicit FlatMemory(std::shared_ptr<const std::vector<uint8_t>> data) : data_(std::move(data)) {}
  SnapshotView::OwnedSpan page(const RegionInfo& region, size_t index) const override {
    const uint64_t address = index == 0 ? region.start : (region.start / page_size + index) * page_size;
    return {address, uint32_t(chunk_length(address, region.end)),
            data_->data() + region.offset + address - region.start, data_};
  }
};
class PagedMemory final : public SnapshotView::Memory {
  const std::vector<PageRef>& pages_;
  mutable std::mutex mutex_;
  mutable hash_type cached_key_ = 0;
  mutable std::shared_ptr<const std::vector<uint8_t>> cached_page_;
 public:
  explicit PagedMemory(const std::vector<PageRef>& pages) : pages_(pages) {}
  SnapshotView::OwnedSpan page(const RegionInfo& region, size_t index) const override {
    const auto& reference = pages_.at(region.offset + index);
    std::lock_guard lock(mutex_);
    if (!cached_page_ || cached_key_ != reference.key) {
      auto bytes = StateStore::instance().load_shared(reference.key);
      require(bool(bytes), "Missing snapshot page");
      require(bytes->size() == reference.length, "Snapshot page length mismatch");
      cached_page_ = std::move(bytes); cached_key_ = reference.key;
    }
    require(cached_page_->size() == reference.length, "Snapshot page length mismatch");
    return {reference.address, reference.length, cached_page_->data(), cached_page_};
  }
};
}

std::vector<uint8_t> encode_page_manifest(const std::vector<RegionInfo>& regions,
    std::span<const uint8_t> metadata, std::string_view maps,
    const user_regs_struct& regs, const std::vector<PageRef>& pages) {
  if (regions.size() > UINT32_MAX || pages.size() > UINT32_MAX ||
      metadata.size() > UINT32_MAX || maps.size() > UINT32_MAX)
    throw std::length_error("Page manifest table exceeds format limits");
  PageManifestHeader h{};
  h.common.magic = 0x4453544D; h.common.version = PAGE_SNAPSHOT_VERSION;
  h.common.num_regions = regions.size();
  h.common.tstate_offset = sizeof(h) + regions.size() * sizeof(RegionInfo);
  h.common.tstate_size = metadata.size();
  h.common.maps_offset = h.common.tstate_offset + metadata.size();
  h.common.maps_size = maps.size();
  h.pages_offset = h.common.maps_offset + maps.size();
  h.page_size = page_size; h.page_count = pages.size();
  h.common.regs_offset = h.pages_offset + pages.size() * sizeof(PageRef);
  require(h.common.regs_offset <= UINT32_MAX - sizeof(regs), "Page manifest exceeds store limit");
  std::vector<uint8_t> result(h.common.regs_offset + sizeof(regs));
  auto copy = [&](uint64_t offset, const void* source, size_t size) {
    if (size) memcpy(result.data() + offset, source, size);
  };
  copy(0, &h, sizeof(h));
  copy(sizeof(h), regions.data(), regions.size() * sizeof(RegionInfo));
  copy(h.common.tstate_offset, metadata.data(), metadata.size());
  copy(h.common.maps_offset, maps.data(), maps.size());
  copy(h.pages_offset, pages.data(), pages.size() * sizeof(PageRef));
  copy(h.common.regs_offset, &regs, sizeof(regs));
  return result;
}

SnapshotView::SnapshotView(std::shared_ptr<const std::vector<uint8_t>> data, uint64_t generation)
    : data_(std::move(data)), generation_(generation) {
  require(data_->size() >= sizeof(header_), "Truncated state snapshot header");
  memcpy(&header_, data_->data(), sizeof(header_));
  require(header_.magic == 0x4453544D && (header_.version == FLAT_SNAPSHOT_VERSION || header_.version == PAGE_SNAPSHOT_VERSION),
          "Incompatible state snapshot version");
  const bool paged = header_.version == PAGE_SNAPSHOT_VERSION;
  const size_t header_size = paged ? sizeof(PageManifestHeader) : sizeof(StateDataHeader);
  auto bounds = [&](uint64_t offset, uint64_t length) {
    return offset <= data_->size() && length <= data_->size() - offset;
  };
  require(data_->size() >= header_size &&
      header_.num_regions <= (data_->size() - header_size) / sizeof(RegionInfo) &&
      header_.tstate_size && bounds(header_.tstate_offset, header_.tstate_size) &&
      bounds(header_.maps_offset, header_.maps_size) &&
      bounds(header_.regs_offset, sizeof(regs_)), "Invalid snapshot section bounds");
  require(header_.tstate_offset == header_size + uint64_t(header_.num_regions) * sizeof(RegionInfo) &&
      header_.maps_offset == header_.tstate_offset + header_.tstate_size &&
      header_.regs_offset >= header_.maps_offset + header_.maps_size &&
      header_.regs_offset + sizeof(regs_) == data_->size(), "Invalid snapshot section ordering");
  regions_.resize(header_.num_regions);
  if (!regions_.empty()) memcpy(regions_.data(), data_->data() + header_size, regions_.size() * sizeof(RegionInfo));
  memcpy(&regs_, data_->data() + header_.regs_offset, sizeof(regs_));
  if (paged) {
    PageManifestHeader h;
    memcpy(&h, data_->data(), sizeof(h));
    require(sysconf(_SC_PAGESIZE) == long(page_size) && h.page_size == page_size,
            "Unsupported page snapshot page size");
    require(h.pages_offset == header_.maps_offset + header_.maps_size &&
        bounds(h.pages_offset, uint64_t(h.page_count) * sizeof(PageRef)) &&
        h.pages_offset + uint64_t(h.page_count) * sizeof(PageRef) == header_.regs_offset,
        "Invalid page manifest section bounds");
    pages_.resize(h.page_count);
    if (!pages_.empty()) memcpy(pages_.data(), data_->data() + h.pages_offset, pages_.size() * sizeof(PageRef));
  }
  uint64_t previous_end = 0;
  uint64_t next_data = paged ? 0 : header_.maps_offset + header_.maps_size;
  for (const auto& region : regions_) {
    const uint32_t prot = PROT_READ | (region.flags[1] == 'w' ? PROT_WRITE : 0) |
        (region.flags[2] == 'x' ? PROT_EXEC : 0);
    require(region.flags[0] == 'r' && (region.flags[1] == 'w' || region.flags[1] == '-') &&
        (region.flags[2] == 'x' || region.flags[2] == '-') && region.flags[3] == 'p' &&
        region.flags[4] == 0 && region.prot == prot, "Invalid snapshot memory region protection");
    require(region.end > region.start && region.start >= previous_end && region.offset == next_data,
            "Invalid snapshot memory region");
    previous_end = region.end;
    if (!paged) {
      require(region.offset <= header_.regs_offset &&
          region.end - region.start <= header_.regs_offset - region.offset, "Invalid snapshot memory extent");
      next_data += region.end - region.start;
    } else {
      uint64_t address = region.start;
      while (address < region.end) {
        require(next_data < pages_.size(), "Missing page manifest coverage");
        const auto& page = pages_[next_data++];
        const auto length = chunk_length(address, region.end);
        require(page.address == address && page.length == length && page.key != 0,
                "Invalid page manifest coverage or key");
        address += length;
      }
    }
  }
  require(next_data == (paged ? pages_.size() : header_.regs_offset), "Invalid snapshot memory extent");
  if (paged) memory_ = std::make_shared<PagedMemory>(pages_);
  else memory_ = std::make_shared<FlatMemory>(data_);
}

void SnapshotView::check_generation() const {
  require(StateStore::instance().generation() == generation_, "Snapshot view belongs to a closed store");
}
std::shared_ptr<const SnapshotView> SnapshotView::open(hash_type hash) {
  auto& store = StateStore::instance();
  uint64_t generation = store.snapshot_generation();
  {
    std::lock_guard lock(cache_mutex);
    if (cache_generation != generation) { cache.clear(); cache_bytes = 0; cache_generation = generation; }
    for (auto it = cache.begin(); it != cache.end(); ++it) {
      if (it->hash == hash) {
        auto result = it->view;
        cache.splice(cache.begin(), cache, it);
        return result;
      }
    }
  }
  auto bytes = store.load_shared(hash);
  require(bool(bytes), "Missing state snapshot");
  uint64_t loaded_generation = store.snapshot_generation();
  require(generation == loaded_generation, "Store changed while opening snapshot");
  // StateStore validates content keys before publishing immutable loaded buffers.
  auto result = std::shared_ptr<const SnapshotView>(new SnapshotView(bytes, loaded_generation));
  const size_t reader_size = result->header_.version == PAGE_SNAPSHOT_VERSION ? sizeof(PagedMemory) : sizeof(FlatMemory);
  const size_t size = sizeof(SnapshotView) + reader_size + bytes->size() +
      result->regions_.size() * sizeof(RegionInfo) + result->pages_.size() * sizeof(PageRef) + page_size;
  if (size <= cache_limit) {
    std::lock_guard lock(cache_mutex);
    if (cache_generation != loaded_generation) { cache.clear(); cache_bytes = 0; cache_generation = loaded_generation; }
    while (!cache.empty() && (cache.size() >= 256 || cache_bytes > cache_limit - size)) {
      cache_bytes -= cache.back().bytes; cache.pop_back();
    }
    cache.push_front({hash, result, size}); cache_bytes += size;
  }
  return result;
}
std::span<const uint8_t> SnapshotView::metadata() const {
  check_generation();
  return {data_->data() + header_.tstate_offset, size_t(header_.tstate_size)};
}
std::string_view SnapshotView::maps_text() const {
  check_generation();
  return {reinterpret_cast<const char*>(data_->data() + header_.maps_offset), size_t(header_.maps_size)};
}
size_t SnapshotView::page_count(size_t region_index) const {
  check_generation();
  const auto& r = regions_.at(region_index);
  return (r.end - 1) / page_size - r.start / page_size + 1;
}
SnapshotView::OwnedSpan SnapshotView::page(size_t region_index, size_t index) const {
  check_generation();
  const auto& r = regions_.at(region_index);
  require(index < page_count(region_index), "Snapshot page index out of range");
  auto result = memory_->page(r, index);
  check_generation();
  return result;
}
void SnapshotView::read_into(uint64_t address, void* destination, size_t length) const {
  check_generation();
  require(length == 0 || destination != nullptr, "Null snapshot read destination");
  require(length <= UINT64_MAX - address, "Snapshot read address overflow");
  auto* out = static_cast<uint8_t*>(destination);
  while (length) {
    auto it = std::upper_bound(regions_.begin(), regions_.end(), address,
        [](uint64_t value, const RegionInfo& r) { return value < r.start; });
    require(it != regions_.begin(), "Snapshot read outside captured regions");
    --it;
    require(address < it->end, "Snapshot read outside captured regions");
    const size_t region = it - regions_.begin();
    const size_t index = address / page_size - it->start / page_size;
    const auto chunk = page(region, index);
    const size_t offset = address - chunk.address;
    const size_t count = std::min<size_t>(length, chunk.length - offset);
    memcpy(out, chunk.data + offset, count);
    address += count; out += count; length -= count;
  }
}
