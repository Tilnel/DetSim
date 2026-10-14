#pragma once
// Experimental only: fixed, stopped tracees. Cache is raw (before normalization),
// not semantic state. No guarantee for pinned pages or external/kernel writers.
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/uio.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace detsim::experiment {
struct Region { uintptr_t address; size_t size; uint8_t* data; };
enum class Mode { delta, verify };
struct SoftDirtyStats {
  uint64_t calls=0, full_fallbacks=0, pages=0, dirty_pages=0;
  uint64_t bytes_read=0, full_bytes=0, scan_us=0, clear_us=0, oracle_us=0, missed_pages=0;
  uint64_t baseline_copy_bytes=0;
};
class SoftDirtyCapture {
  using Clock=std::chrono::steady_clock;
  struct Baseline { std::string layout; std::vector<std::pair<uintptr_t,size_t>> ranges; std::vector<uint8_t> bytes; };
  struct Fd {
    int value;
    explicit Fd(const std::string& path,int flags):value(open(path.c_str(),flags|O_CLOEXEC)) {
      if(value<0) throw std::runtime_error("soft-dirty open " + path + ": " + strerror(errno));
    }
    ~Fd(){close(value);}
  };
  Mode mode_; SoftDirtyStats stats_; long page_size_;
  std::unordered_map<pid_t,Baseline> baselines_;
  static uint64_t elapsed(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-start).count();
  }
  static void exact_read(pid_t pid,const std::vector<Region>& ranges) {
    for(size_t begin=0;begin<ranges.size();begin+=256) {
      std::array<iovec,256> local{},remote{}; size_t count=std::min(size_t(256),ranges.size()-begin), total=0;
      for(size_t i=0;i<count;++i) {
        const auto& r=ranges[begin+i];
        if(r.size>size_t(SSIZE_MAX)-total) throw std::runtime_error("soft-dirty read overflow");
        total+=r.size; local[i]={r.data,r.size}; remote[i]={reinterpret_cast<void*>(r.address),r.size};
      }
      ssize_t got;
      do {got=process_vm_readv(pid,local.data(),count,remote.data(),count,0);} while(got<0 && errno==EINTR);
      if(got<0 || size_t(got)!=total) throw std::runtime_error("soft-dirty process_vm_readv failed or short");
    }
  }
public:
  explicit SoftDirtyCapture(Mode mode):mode_(mode),page_size_(sysconf(_SC_PAGESIZE)) {
    if(page_size_<=0 || (page_size_&(page_size_-1))) throw std::runtime_error("soft-dirty invalid page size");
  }
  const SoftDirtyStats& stats() const {return stats_;}
  void reset_stats() {stats_ = {}; }
  // Caller stops every thread and supplies the identical full mapping descriptor
  // each capture. A changed layout forces a full read. PIDs must not be reused.
  void capture(pid_t pid,const std::vector<Region>& ranges,const std::string& layout) {
    ++stats_.calls; size_t total=0;
    std::vector<std::pair<uintptr_t,size_t>> keys;
    for(const auto& r:ranges) {
      if(!r.size || r.address>UINTPTR_MAX-r.size ||
         r.address+r.size>UINTPTR_MAX-uintptr_t(page_size_-1) || total>SIZE_MAX-r.size)
        throw std::runtime_error("soft-dirty invalid range");
      total+=r.size; keys.emplace_back(r.address,r.size);
    }
    stats_.full_bytes+=total;
    auto& baseline=baselines_[pid];
    bool eligible=baseline.layout==layout && baseline.ranges==keys && baseline.bytes.size()==total;
    std::vector<Region> reads;
    std::vector<size_t> read_offsets; size_t offset=0;
    if(eligible) {
      const auto start=Clock::now();
      Fd pagemap("/proc/"+std::to_string(pid)+"/pagemap",O_RDONLY);
      for(const auto& r:ranges) {
        const size_t region_offset=offset, first_read=reads.size();
        memcpy(r.data,baseline.bytes.data()+offset,r.size); offset+=r.size;
        const uintptr_t end=r.address+r.size;
        uintptr_t page=r.address & ~(uintptr_t(page_size_)-1);
        while(page<end) {
          size_t count=std::min(size_t(256),size_t((end-1-page)/page_size_+1));
          std::array<uint64_t,256> entries{};
          uint64_t index=page/uint64_t(page_size_);
          if(index>uint64_t(std::numeric_limits<off_t>::max())/8)
            throw std::runtime_error("soft-dirty pagemap offset overflow");
          ssize_t got;
          do {got=pread(pagemap.value,entries.data(),count*8,off_t(index*8));} while(got<0 && errno==EINTR);
          if(got<0 || size_t(got)!=count*8) throw std::runtime_error("soft-dirty pagemap failed or short");
          for(size_t i=0;i<count;++i) {
            uintptr_t low=std::max(page,r.address), high=std::min(end,page+uintptr_t(page_size_));
            ++stats_.pages;
            // Nonpresent and swapped entries are conservative reads.
            if((entries[i]&(uint64_t(1)<<55)) || !(entries[i]&(uint64_t(1)<<63))) {
              ++stats_.dirty_pages;
              Region next{low,size_t(high-low),r.data+(low-r.address)};
              if(reads.size()>first_read && reads.back().address+reads.back().size==next.address &&
                 reads.back().data+reads.back().size==next.data) reads.back().size+=next.size;
              else {reads.push_back(next);read_offsets.push_back(region_offset+low-r.address);}
            }
            page+=page_size_;
          }
        }
      }
      stats_.scan_us+=elapsed(start);
    } else {
      ++stats_.full_fallbacks; reads=ranges;
      for(const auto& r:ranges) {read_offsets.push_back(offset);offset+=r.size;}
      for(const auto& r:ranges) {
        uint64_t count=(r.address%page_size_+r.size-1)/page_size_+1;
        stats_.pages+=count; stats_.dirty_pages+=count;
      }
    }
    exact_read(pid,reads);
    for(const auto& r:reads) stats_.bytes_read+=r.size;
    if(mode_==Mode::verify && eligible) {
      const auto start=Clock::now(); std::vector<uint8_t> oracle(total); std::vector<Region> oracle_ranges;
      offset=0;
      for(const auto& r:ranges) {oracle_ranges.push_back({r.address,r.size,oracle.data()+offset}); offset+=r.size;}
      exact_read(pid,oracle_ranges); offset=0;
      for(const auto& r:ranges) {
        for(size_t pos=0;pos<r.size;) {
          size_t length=std::min(r.size-pos,size_t(page_size_-(r.address+pos)%page_size_));
          if(memcmp(r.data+pos,oracle.data()+offset+pos,length)) ++stats_.missed_pages;
          pos+=length;
        }
        offset+=r.size;
      }
      stats_.oracle_us+=elapsed(start);
      if(stats_.missed_pages) throw std::runtime_error("soft-dirty missed changed raw page");
    }
    baseline.layout=layout; baseline.ranges=std::move(keys); baseline.bytes.resize(total);
    // All unselected bytes already equal the preceding raw baseline. Commit only
    // reads, indexed independently of caller buffers and virtual address gaps.
    for(size_t i=0;i<reads.size();++i) {
      memcpy(baseline.bytes.data()+read_offsets[i],reads[i].data,reads[i].size);
      stats_.baseline_copy_bytes+=reads[i].size;
    }
    const auto start=Clock::now();
    Fd clear("/proc/"+std::to_string(pid)+"/clear_refs",O_WRONLY);
    ssize_t written;
    do {written=write(clear.value,"4",1);} while(written<0 && errno==EINTR);
    if(written!=1) {baselines_.erase(pid);throw std::runtime_error("soft-dirty clear_refs failed or short");}
    stats_.clear_us+=elapsed(start);
  }
  void report() const {
    fprintf(stderr,"SOFT_DIRTY {\"calls\":%llu,\"full_fallbacks\":%llu,\"pages\":%llu,\"dirty_pages\":%llu,\"bytes_read\":%llu,\"full_bytes\":%llu,\"scan_us\":%llu,\"clear_us\":%llu,\"oracle_us\":%llu,\"missed_pages\":%llu,\"baseline_copy_bytes\":%llu}\n",
      (unsigned long long)stats_.calls,(unsigned long long)stats_.full_fallbacks,
      (unsigned long long)stats_.pages,(unsigned long long)stats_.dirty_pages,
      (unsigned long long)stats_.bytes_read,(unsigned long long)stats_.full_bytes,
      (unsigned long long)stats_.scan_us,(unsigned long long)stats_.clear_us,
      (unsigned long long)stats_.oracle_us,(unsigned long long)stats_.missed_pages,
      (unsigned long long)stats_.baseline_copy_bytes);
  }
};
// No tracker construction or proc operations when unset. Fixed for process lifetime.
inline SoftDirtyCapture* configured_capture() {
  static const char* mode=getenv("DETSIM_SOFT_DIRTY_EXPERIMENT");
  if(!mode) return nullptr;
  if(strcmp(mode,"delta") && strcmp(mode,"verify")) throw std::runtime_error("Invalid DETSIM_SOFT_DIRTY_EXPERIMENT");
  struct Reported {
    SoftDirtyCapture capture;
    explicit Reported(Mode m):capture(m){}
    ~Reported(){capture.report();}
  };
  static Reported tracker(!strcmp(mode,"verify")?Mode::verify:Mode::delta);
  return &tracker.capture;
}
} // namespace detsim::experiment
