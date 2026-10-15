#include "fsstate.h"
#include "state_store.h"
#include "state_store_packed.h"
#include "ncurses_ui.h"
#include "cereal/archives/binary.hpp"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

extern "C" detsim::ui::NCursesUI* get_ncurses_ui() { return nullptr; }
static std::atomic<bool> fail_sync{false};
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
    if (fail_sync.load()) { errno=EIO; return -1; }
    return __real_fsync(fd);
}
static void require(bool value,const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F> static void rejects(F action) {
    bool failed=false;
    try { action(); } catch (const std::exception&) { failed=true; }
    require(failed,"Expected explicit rejection");
}
static std::string encode(const VFSNode& node,bool references=false) {
    auto serialize=[&] {
        std::ostringstream output;
        { cereal::BinaryOutputArchive ar(output); ar(node); }
        return output.str();
    };
    if (references) { FileContentSnapshotScope scope; return serialize(); }
    return serialize();
}
static VFSNode decode(const std::string& bytes) {
    VFSNode result{};
    std::istringstream input(bytes);
    { cereal::BinaryInputArchive ar(input); ar(result); }
    require(input.peek()==std::char_traits<char>::eof(),"Trailing file bytes");
    return result;
}
static void expected(const VFSNode& node) {
    require(node.content.size()==1024*1024 && node.content[0]=='A' &&
            node.content[node.content.size()-1]=='A',"Saved file payload bytes");
}
int main(int argc,char** argv) {
    auto& store=StateStore::instance();
    if (argc==4 && !strcmp(argv[1],"--read")) {
        try {
            StateStore::Config cfg; cfg.packed_storage_path=argv[2]; cfg.read_only=true;
            cfg.hot_cache_size=0; cfg.warm_cache_size=0; cfg.prefetch_threads=0;
            store.init(cfg);
            std::ifstream file(argv[3],std::ios::binary);
            std::string wire((std::istreambuf_iterator<char>(file)),{});
            expected(decode(wire)); store.shutdown(); return 0;
        } catch(const std::exception& error) { fprintf(stderr,"reader: %s\n",error.what()); return 1; }
    }
    char directory[]="/tmp/detsim-vfs-content-XXXXXX";
    require(mkdtemp(directory),"mkdtemp");
    try {
        VFSNode original{}; original.metadata.st_mode=S_IFREG|0600;
        original.content.assign(1024*1024,'A'); original.metadata.st_size=original.content.size();
        auto branch=original;
        require(branch.content.data()==original.content.data(),"Copied payload is shared");
        branch.content.write(0,"B",1);
        require(branch.content[0]=='B' && original.content[0]=='A',"Write isolates branches");
        branch=original; branch.content.clear();
        require(branch.content.empty() && original.content.size()==1024*1024,"Clear isolates branches");
        FileContent overlap; overlap={'a','b','c','d'};
        overlap.write(1,overlap.data(),3);
        require(std::string(overlap.begin(),overlap.end())=="aabc","Self-aliasing write retains source bytes");
        FileSystemState live(std::make_shared<FdManager>());
        live.filesystem["/file"]=original;
        const int fd=live.do_open("/file",O_RDWR,0);
        require(fd>=0,"Open shared file");
        const auto saved=live;
        require(live.do_write(fd,"Z",1)==1 && saved.filesystem.at("/file").content[0]=='A',"Filesystem write isolates saved state");
        require(live.do_open("/file",O_WRONLY|O_TRUNC,0)>=0 && live.filesystem.at("/file").content.empty(),"Filesystem truncation");
        expected(saved.filesystem.at("/file"));
        const auto inline_wire=encode(original);
        require(inline_wire.size()>original.content.size(),"Default archive contains payload");
        expected(decode(inline_wire));
        try { FileContentSnapshotScope outer; FileContentSnapshotScope inner; throw 1; } catch(int) {}
        require(encode(original)==inline_wire,"Exception restores inline archive mode");
        StateStore::Config cfg; cfg.packed_storage_path=std::string(directory)+"/packed";
        cfg.hot_cache_size=0; cfg.warm_cache_size=0; cfg.prefetch_threads=0;
        store.init(cfg);
        const auto wire=encode(original,true);
        require(wire.size()<128,"Reference metadata does not scale with file size");
        const auto saves=store.get_stats().save_calls;
        for(int i=0;i<10;++i) require(encode(original,true)==wire,"Stable reference bytes");
        require(store.get_stats().save_calls==saves,"Unchanged payload registered only once");
        VFSNode independent{}; independent.metadata=original.metadata;
        independent.content.assign(original.content.size(),'A');
        require(encode(independent,true)==wire,"Identity independent of sharing and registration order");
        expected(decode(wire));
        auto restored=decode(wire);
        restored.content.write(0,"B",1);
        require(restored.content[0]=='B',"Restored payload can detach for mutation");
        expected(decode(wire));
        require(encode(restored,true)!=wire,"Mutated restored reference has new identity");
        auto registered=independent;
        independent.content.clear(); // leave registered as sole payload owner
        registered.content.write(0,"C",1);
        require(encode(registered,true)!=wire && decode(encode(registered,true)).content[0]=='C',"Unique registered write invalidates memo");
        expected(decode(wire));
        VFSNode small{}; small.content.assign(4095,'s'); small.metadata.st_size=4095;
        require(encode(small,true)==encode(small),"Small payload remains canonical inline");
        auto bad=wire; bad[0]=127; rejects([&]{decode(bad);});
        uint64_t length=2; bad=wire; memcpy(bad.data()+1,&length,sizeof(length));
        rejects([&]{decode(bad);});
        length=original.content.size()+1; bad=wire; memcpy(bad.data()+1,&length,sizeof(length));
        rejects([&]{decode(bad);});
        hash_type key=UINT64_MAX; bad=wire; memcpy(bad.data()+9,&key,sizeof(key));
        rejects([&]{decode(bad);});
        store.wait_for_completion();
        const auto wire_path=std::string(directory)+"/node.bin";
        { std::ofstream output(wire_path,std::ios::binary); output.write(wire.data(),wire.size()); }
        const pid_t reader=fork(); require(reader>=0,"fork");
        if (!reader) { execl("/proc/self/exe","vfs-reader","--read",cfg.packed_storage_path.c_str(),wire_path.c_str(),nullptr); _exit(127); }
        int status=0;
        require(waitpid(reader,&status,0)==reader && WIFEXITED(status) && !WEXITSTATUS(status),"Fresh-process reference load");
        // A checksummed packed record under a false key must not become content.
        key=0x1a23456789abcdefULL;
        StateStorePacked::instance().save(original.content.data(),original.content.size(),key);
        bad=wire; memcpy(bad.data()+9,&key,sizeof(key)); rejects([&]{decode(bad);});
        store.shutdown();
        expected(decode(inline_wire)); rejects([&]{decode(wire);});
        cfg.read_only=true; store.init(cfg); expected(decode(wire)); store.shutdown();
        // The same live payload must register into a different store generation.
        cfg.read_only=false; cfg.packed_storage_path=std::string(directory)+"/second";
        store.init(cfg); require(encode(original,true)==wire,"Cross-generation reference identity");
        store.wait_for_completion(); expected(decode(wire));
        fail_sync=true; rejects([&]{store.wait_for_completion();}); fail_sync=false;
        rejects([&]{encode(original,true);});
        try { store.shutdown(); } catch(const std::exception&) {}
        cfg.packed_storage_path=std::string(directory)+"/collision";
        store.init(cfg);
        memcpy(&key,wire.data()+9,sizeof(key));
        StateStorePacked::instance().save("wrong",5,key);
        rejects([&]{encode(original,true);});
        try { store.shutdown(); } catch(const std::exception&) {}
        std::filesystem::remove_all(directory);
        puts("PASS shared VFS payload, reference identity, cold reopen, corruption and poison");
        return 0;
    } catch(const std::exception& error) {
        fail_sync=false; try { store.shutdown(); } catch(...) {}
        fprintf(stderr,"FAIL VFS content: %s\n",error.what()); return 1;
    }
}
