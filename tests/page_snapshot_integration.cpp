#include "page_snapshot.h"
#include "state_store_packed.h"
#include "debug.h"
#include "ncurses_ui.h"
#include <cstdio>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/wait.h>
static std::atomic<bool> fail_sync{false};
extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd) {
  if (fail_sync.load()) { errno = EIO; return -1; }
  return __real_fsync(fd);
}
extern "C" detsim::ui::NCursesUI* get_ncurses_ui() { return nullptr; }
static void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class F> static void rejects(F f) {
  bool rejected = false;
  try { f(); } catch (const std::exception&) { rejected = true; }
  require(rejected, "Expected explicit rejection");
}
static RegionInfo region(uint64_t start, uint64_t end, uint64_t offset) {
  RegionInfo r{}; r.start = start; r.end = end; r.offset = offset;
  r.prot = PROT_READ | PROT_WRITE; memcpy(r.flags, "rw-p", 5); return r;
}
int main(int argc, char** argv) {
  if (argc == 4 && !strcmp(argv[1], "--read")) {
    try {
      StateStore::Config cfg; cfg.read_only=true; cfg.packed_storage_path=argv[2];
      cfg.hot_cache_size=1; cfg.warm_cache_size=1;
      StateStore::instance().init(cfg);
      const auto root=std::stoull(argv[3],nullptr,16);
      auto view=SnapshotView::open(root);
      std::vector<uint8_t> output(8192);
      view->read_into(0x1003,output.data(),output.size());
      for (size_t i=0;i<output.size();++i)
        require(output[i]==uint8_t(i*37),"Fresh-process page bytes");
      StateStore::instance().shutdown();
      return 0;
    } catch (const std::exception& error) {
      fprintf(stderr,"fresh page reader: %s\n",error.what()); return 1;
    }
  }
  char directory[] = "/tmp/detsim-pages-XXXXXX";
  require(mkdtemp(directory) != nullptr, "mkdtemp");
  auto& store = StateStore::instance();
  try {
    StateStore::Config cfg; cfg.packed_storage_path = std::string(directory) + "/packed";
    cfg.hot_cache_size = 1; cfg.warm_cache_size = 1; cfg.prefetch_threads = 0;
    store.init(cfg);
    std::vector<uint8_t> bytes(8192); for (size_t i=0; i<bytes.size(); ++i) bytes[i] = i * 37;
    auto save = [&](const std::vector<uint8_t>& data) { return store.save(data.data(), data.size()); };
    // Unaligned first region, followed by adjacent region: chunks stop at each boundary.
    std::vector<RegionInfo> regions{region(0x1003, 0x2800, 0), region(0x2800, 0x3003, 2)};
    std::vector<PageRef> pages;
    uint64_t cursor = 0;
    for (auto& r : regions) {
      r.offset = pages.size();
      for (uint64_t address = r.start; address < r.end;) {
        uint32_t size = std::min<uint64_t>(r.end-address, 4096-address%4096);
        auto key = store.save(bytes.data()+cursor, size);
        pages.push_back({address,size,key}); address += size; cursor += size;
      }
    }
    std::vector<uint8_t> metadata{1,2,3}; user_regs_struct regs{}; regs.rip = 42;
    auto manifest = encode_page_manifest(regions,metadata,"maps",regs,pages);
    auto key = save(manifest); store.wait_for_completion();
    for (uint32_t unsupported : {5u,6u}) {
      auto older=manifest;
      reinterpret_cast<StateDataHeader*>(older.data())->version=unsupported;
      rejects([&]{SnapshotView::open(save(older));});
    }
    char root_argument[32]; snprintf(root_argument,sizeof(root_argument),"%llx",(unsigned long long)key);
    const auto reader=fork(); require(reader>=0,"fork fresh reader");
    if (!reader) {
      execl("/proc/self/exe","page-reader","--read",cfg.packed_storage_path.c_str(),root_argument,nullptr);
      _exit(127);
    }
    int reader_status;
    require(waitpid(reader,&reader_status,0)==reader && WIFEXITED(reader_status) &&
        WEXITSTATUS(reader_status)==0,"Fresh-process read while writer alive");
    auto view = SnapshotView::open(key);
    require(view->format_version()==PAGE_SNAPSHOT_VERSION && view->main_regs().rip==42 && view->maps_text()=="maps", "Manifest metadata");
    require(view->metadata().size()==3 && view->page_count(0)==2 && view->page_count(1)==2, "Manifest tables");
    std::vector<uint8_t> output(bytes.size()); view->read_into(0x1003, output.data(), output.size());
    require(output==bytes, "Cross-page and adjacent-region read");
    auto pinned = view->page(0,0); view->page(1,1);
    require(pinned.data[0]==bytes[0], "Pinned page ownership");
    rejects([&]{view->read_into(0x3003,output.data(),1);});
    rejects([&]{view->page(0,2);});
    auto bad = manifest; reinterpret_cast<PageManifestHeader*>(bad.data())->page_size=8192;
    rejects([&]{SnapshotView::open(save(bad));});
    bad=manifest; reinterpret_cast<PageManifestHeader*>(bad.data())->common.tstate_size=0;
    rejects([&]{SnapshotView::open(save(bad));});
    bad=manifest; reinterpret_cast<PageManifestHeader*>(bad.data())->common.regs_offset--;
    rejects([&]{SnapshotView::open(save(bad));});
    auto wrong=pages; wrong[0].address++;
    rejects([&]{SnapshotView::open(save(encode_page_manifest(regions,metadata,"maps",regs,wrong)));});
    wrong=pages; wrong[0].key=UINT64_MAX;
    auto missing=SnapshotView::open(save(encode_page_manifest(regions,metadata,"maps",regs,wrong)));
    rejects([&]{missing->page(0,0);});
    wrong=pages; wrong[0].key=store.save(metadata.data(),metadata.size());
    auto shortpage=SnapshotView::open(save(encode_page_manifest(regions,metadata,"maps",regs,wrong)));
    rejects([&]{shortpage->page(0,0);});
    // Deliberately publish internally checksummed records under false content keys.
    // The view must reject both a corrupt semantic manifest key and a page key.
    store.wait_for_completion();
    auto& packed = StateStorePacked::instance();
    constexpr hash_type false_manifest = 0x123456789abcdef0ULL;
    packed.save(manifest.data(), manifest.size(), false_manifest);
    rejects([&]{SnapshotView::open(false_manifest);});
    constexpr hash_type false_page = 0x123456789abcdef1ULL;
    packed.save(bytes.data(), pages[0].length, false_page);
    wrong=pages; wrong[0].key=false_page;
    auto corrupt=SnapshotView::open(save(encode_page_manifest(regions,metadata,"maps",regs,wrong)));
    rejects([&]{corrupt->page(0,0);});
    // Existing v7 files expose the same decoded bytes without conversion.
    StateDataHeader h{}; h.magic=0x4453544D; h.version=FLAT_SNAPSHOT_VERSION; h.num_regions=regions.size();
    h.tstate_offset=sizeof(h)+regions.size()*sizeof(RegionInfo); h.tstate_size=metadata.size();
    h.maps_offset=h.tstate_offset+h.tstate_size; h.maps_size=4; h.regs_offset=h.maps_offset+4+bytes.size();
    std::vector<uint8_t> legacy(h.regs_offset+sizeof(regs)); memcpy(legacy.data(),&h,sizeof(h));
    uint64_t offset=h.maps_offset+h.maps_size;
    for(size_t i=0;i<regions.size();++i) { auto r=regions[i]; r.offset=offset; offset+=r.end-r.start; memcpy(legacy.data()+sizeof(h)+i*sizeof(r),&r,sizeof(r)); }
    memcpy(legacy.data()+h.tstate_offset,metadata.data(),metadata.size()); memcpy(legacy.data()+h.maps_offset,"maps",4);
    memcpy(legacy.data()+h.maps_offset+4,bytes.data(),bytes.size()); memcpy(legacy.data()+h.regs_offset,&regs,sizeof(regs));
    auto old=SnapshotView::open(save(legacy)); old->read_into(0x1003,output.data(),output.size());
    require(output==bytes && old->format_version()==FLAT_SNAPSHOT_VERSION,"Legacy adapter");
    store.wait_for_completion(); store.shutdown();
    rejects([&]{view->page(0,0);});
    cfg.read_only=true; store.init(cfg); auto reopened=SnapshotView::open(key);
    reopened->read_into(0x1003,output.data(),output.size()); require(output==bytes,"Cold reopen");
    rejects([&]{view->metadata();}); store.shutdown();
    // Cached v7 bytes and v8 pages must not bypass a poisoned store.
    cfg.read_only=false; cfg.hot_cache_size=65536; cfg.warm_cache_size=65536;
    store.init(cfg);
    const auto legacy_key=save(legacy);
    auto warm_old=SnapshotView::open(legacy_key);
    auto warm_new=SnapshotView::open(key);
    warm_old->page(0,0); warm_new->page(0,0);
    require(SnapshotView::open(key)==warm_new, "Parsed cache not warmed");
    fail_sync=true;
    rejects([&]{store.wait_for_completion();});
    fail_sync=false;
    for (const auto& poisoned : {warm_old,warm_new}) {
      rejects([&]{poisoned->read_into(0x1003,output.data(),1);});
      rejects([&]{poisoned->page(0,0);});
      rejects([&]{poisoned->metadata();});
      rejects([&]{poisoned->maps_text();});
      rejects([&]{poisoned->regions();});
      rejects([&]{poisoned->main_regs();});
      rejects([&]{poisoned->format_version();});
      rejects([&]{poisoned->page_count(0);});
    }
    rejects([&]{SnapshotView::open(key);});
    rejects([&]{SnapshotView::open(legacy_key);});
    rejects([&]{store.shutdown();});
    std::filesystem::remove_all(directory);
    puts("page_snapshot_integration: PASS (cold/tiny, v7/v8, coverage, bounds, missing/length/key integrity, lifecycle/poison)");
    return 0;
  } catch (const std::exception& e) {
    fprintf(stderr,"page_snapshot_integration: %s\n",e.what());
    try {store.shutdown();} catch (...) {}
    std::filesystem::remove_all(directory); return 1;
  }
}
