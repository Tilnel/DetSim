#ifndef DETSIM_PAGE_SNAPSHOT_H
#define DETSIM_PAGE_SNAPSHOT_H
#include "state.h"
#include "state_store.h"
#include <span>
#include <string_view>
#include <mutex>

struct __attribute__((packed)) PageRef {
  uint64_t address;
  uint32_t length;
  uint64_t key;
};
static_assert(sizeof(PageRef) == 20);
struct __attribute__((packed)) PageManifestHeader {
  StateDataHeader common;
  uint32_t page_size;
  uint32_t page_count;
  uint64_t pages_offset;
};
static_assert(sizeof(PageManifestHeader) == 68);
std::vector<uint8_t> encode_page_manifest(const std::vector<RegionInfo>& regions,
    std::span<const uint8_t> metadata, std::string_view maps,
    const user_regs_struct& regs, const std::vector<PageRef>& pages);

class SnapshotView {
 public:
  SnapshotView(const SnapshotView&) = delete;
  SnapshotView& operator=(const SnapshotView&) = delete;
  struct OwnedSpan {
    uint64_t address;
    uint32_t length;
    const uint8_t* data;
    std::shared_ptr<const std::vector<uint8_t>> owner;
  };
  // Backend dispatch occurs once per chunk, never once per byte.
  class Memory;
  static std::shared_ptr<const SnapshotView> open(hash_type hash);
  const std::vector<RegionInfo>& regions() const { check_generation(); return regions_; }
  std::span<const uint8_t> metadata() const;
  std::string_view maps_text() const;
  const user_regs_struct& main_regs() const { check_generation(); return regs_; }
  uint32_t version() const { check_generation(); return header_.version; }
  uint32_t format_version() const { check_generation(); return header_.version; }
  size_t page_count(size_t region) const;
  OwnedSpan page(size_t region, size_t index) const;
  void read_into(uint64_t address, void* destination, size_t length) const;
 private:
  explicit SnapshotView(std::shared_ptr<const std::vector<uint8_t>> data, uint64_t generation);
  void check_generation() const;
  std::shared_ptr<const std::vector<uint8_t>> data_;
  uint64_t generation_;
  StateDataHeader header_{};
  std::vector<RegionInfo> regions_;
  std::vector<PageRef> pages_;
  user_regs_struct regs_{};
  std::shared_ptr<const Memory> memory_;
};
#endif
