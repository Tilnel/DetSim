#include "config.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include "guest.h"
#include "io.h"
#include "dispatcher.h"
#include "handlers.h"
#include "fsstate.h"
#include "sockstate.h"
#include "cereal/archives/binary.hpp"
#include <cstdio>
#include <array>
#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>

static pollfd guest_pollfds[8]{};
static sockaddr_in guest_address{};
static char guest_bytes[32]="abc";
static epoll_event guest_event{}, guest_events[4]{};
static int guest_real_fd=-1, guest_pipefds[2]{}, guest_integer{};
static struct stat guest_stat{};
static_assert(NP == 2);
extern "C" detsim::ui::NCursesUI* get_ncurses_ui() {return nullptr;}
static int failures=0;
static void check(bool ok,const char* label) {
    printf("%s %s\n",ok?"PASS":"FAIL",label); if(!ok) ++failures;
}
static void copy_to(pid_t pid, void* target,const void* source,size_t size) {
    iovec local{const_cast<void*>(source),size},remote{target,size};
    if(process_vm_writev(pid,&local,1,&remote,1,0)!=ssize_t(size)) throw std::runtime_error("fixture copyout");
}
template<class T> static std::string archive(T& object) {
    std::ostringstream output; {cereal::BinaryOutputArchive a(output);a(object);} return output.str();
}
static void reset() {
    for(int i=0;i<NP;++i) {
        ptmc_state.fd_managers[i]=std::make_shared<FdManager>();
        ptmc_state.fs_states[i]=FileSystemState();
        ptmc_state.sock_states[i]=SockState();
        ptmc_state.fs_states[i].set_fd_manager(ptmc_state.fd_managers[i]);
        ptmc_state.sock_states[i].set_fd_manager(ptmc_state.fd_managers[i]);
        ptmc_state.addrs[i]=i?"192.168.0.2":"192.168.0.1";
    }
    ptmc_state.cursor=0;
}
static std::pair<int,int> connected(bool accept=true) {
    reset();
    const int listener=ptmc_state.sock_states[0].do_socket(AF_INET,SOCK_STREAM,0);
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(9000);
    inet_pton(AF_INET,"192.168.0.1",&address.sin_addr);
    copy_to(ptmc_state.pids[0],&guest_address,&address,sizeof(address));
    if(ptmc_state.sock_states[0].do_bind(listener,reinterpret_cast<sockaddr*>(&guest_address),sizeof(address)) ||
       ptmc_state.sock_states[0].do_listen(listener,8)) throw std::runtime_error("listen fixture");
    ptmc_state.cursor=1;
    int client=ptmc_state.sock_states[1].do_socket(AF_INET,SOCK_STREAM,0);
    copy_to(ptmc_state.pids[1],&guest_address,&address,sizeof(address));
    if(ptmc_state.sock_states[1].do_connect(client,reinterpret_cast<sockaddr*>(&guest_address),sizeof(address)))
        throw std::runtime_error("connect fixture");
    ptmc_state.cursor=0;
    int server=accept?ptmc_state.sock_states[0].do_accept(listener,nullptr,nullptr):listener;
    if(server<0) throw std::runtime_error("accept fixture");
    return {client,server};
}
static long execute(SyscallDispatcher& dispatcher,pid_t pid,int number,
                    std::array<uint64_t,6> arguments,user_regs_struct* returned=nullptr) {
    user_regs_struct original,registers;
    if(ptrace(PTRACE_GETREGS,pid,nullptr,&original)) throw std::runtime_error("read registers");
    registers=original;registers.rip=scratch_page+0xff0;registers.rax=number;
    registers.rdi=arguments[0];registers.rsi=arguments[1];registers.rdx=arguments[2];
    registers.r10=arguments[3];registers.r8=arguments[4];registers.r9=arguments[5];
    if(ptrace(PTRACE_SETREGS,pid,nullptr,&registers)) throw std::runtime_error("set syscall");
    auto advance=[&] {
        int status;
        if(ptrace(PTRACE_SYSCALL,pid,nullptr,nullptr) || waitpid(pid,&status,0)!=pid ||
           !WIFSTOPPED(status) || WSTOPSIG(status)!=(SIGTRAP|0x80)) throw std::runtime_error("syscall stop");
    };
    advance();syscall_info info{};info.nr=number;
    memcpy(info.args,arguments.data(),sizeof(info.args));
    dispatcher.on_enter(pid,number,info);advance();
    ptrace(PTRACE_GETREGS,pid,nullptr,&registers);info.rval=registers.rax;
    dispatcher.on_exit(pid,info);ptrace(PTRACE_GETREGS,pid,nullptr,&registers);
    if(returned) *returned=registers;
    ptrace(PTRACE_SETREGS,pid,nullptr,&original);
    return static_cast<long>(registers.rax);
}
static bool mapping_entry_rejected(SyscallDispatcher& dispatcher,pid_t pid,int number,
                                   uint64_t address,uint64_t length,uint64_t operation) {
    user_regs_struct before{},after{};
    if(ptrace(PTRACE_GETREGS,pid,nullptr,&before)) throw std::runtime_error("read admission registers");
    syscall_info info{};info.nr=number;info.args[0]=address;info.args[1]=length;info.args[2]=operation;
    bool rejected=false;
    try {dispatcher.on_enter(pid,number,info);}
    catch(const std::runtime_error&) {rejected=true;}
    if(ptrace(PTRACE_GETREGS,pid,nullptr,&after)) throw std::runtime_error("read rejected registers");
    if(memcmp(&before,&after,sizeof before)) throw std::runtime_error("mapping admission changed registers");
    return rejected;
}
int main() {
    pid_t children[NP]{};
    try {
        for(int i=0;i<NP;++i) {
            children[i]=fork(); if(children[i]<0) throw std::runtime_error("fork");
            if(!children[i]) {
                guest_real_fd=open("/dev/null",O_RDWR);
                if(mmap(reinterpret_cast<void*>(scratch_page),4096,PROT_READ|PROT_WRITE|PROT_EXEC,
                     MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==MAP_FAILED) _exit(2);
                auto* code=reinterpret_cast<unsigned char*>(scratch_page+0xff0);code[0]=0x0f;code[1]=0x05;
                if(ptrace(PTRACE_TRACEME,0,nullptr,nullptr)) _exit(2);
                asm volatile("int3");_exit(0);
            }
            int status; if(waitpid(children[i],&status,0)!=children[i] || !WIFSTOPPED(status)) throw std::runtime_error("stop");
            ptmc_state.pids[i]=children[i];
            if(ptrace(PTRACE_SETOPTIONS,children[i],nullptr,PTRACE_O_TRACESYSGOOD|PTRACE_O_EXITKILL)) throw std::runtime_error("options");
        }
        reset();
        auto& fs=ptmc_state.fs_states[0];
        int file=fs.do_open("/file",O_CREAT|O_RDWR,0600);
        int socket=ptmc_state.sock_states[0].do_socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0);
        check(ptmc_state.fd_managers[0]->get_fd_type(file)==FdType::FILE &&
              ptmc_state.fd_managers[0]->get_fd_type(socket)==FdType::SOCKET,"typed shared FD namespace");
        check(emu_close(file)==0 && !ptmc_state.fd_managers[0]->is_allocated(file),"file close releases descriptor");
        check(fs.do_open("/second",O_CREAT|O_RDWR,0600)==file,"lowest closed fd reused");
        check((emu_fcntl(socket,F_GETFL,0)&O_NONBLOCK)!=0,"socket flags survive F_GETFL");
        SyscallDispatcher dispatcher;
        check(dispatcher.get_route(SYS_fcntl).action==SyscallAction::EMULATE &&
              dispatcher.get_route(SYS_fsync).action==SyscallAction::EMULATE,"entry blocks FD control syscalls");
        check(dispatcher.get_route(SYS_poll).action==SyscallAction::EMULATE,
              "poll is suppressed and emulated");
        pollfd probe[4]{{file,POLLIN|POLLOUT,123},{socket,POLLOUT,123},{-1,POLLIN,123},{99999,0,123}};
        copy_to(children[0],guest_pollfds,probe,sizeof probe);
        check(execute(dispatcher,children[0],SYS_poll,
                      {reinterpret_cast<uint64_t>(guest_pollfds),(1ULL<<32)|4,1ULL<<32,0,0,0})==3 &&
              guest_read_exact(children[0],guest_pollfds,probe,sizeof probe) &&
              probe[0].revents==(POLLIN|POLLOUT) && probe[1].revents==POLLOUT &&
              probe[2].revents==0 && probe[3].revents==POLLNVAL,
              "poll typed readiness, negative/invalid descriptors and ABI truncation");
        check(emu_poll(reinterpret_cast<pollfd*>(1),1,0)==-EFAULT && emu_poll(nullptr,0,0)==0,
              "poll checked input and empty array");
        const long readonly_poll=tracee_do_syscall(children[0],SYS_mmap,0,4096,
            PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if (readonly_poll<0) throw std::runtime_error("poll copyout fixture mmap");
        copy_to(children[0],reinterpret_cast<void*>(readonly_poll),probe,sizeof probe);
        tracee_do_syscall(children[0],SYS_mprotect,readonly_poll,4096,PROT_READ,0,0,0);
        check(emu_poll(reinterpret_cast<pollfd*>(readonly_poll),1,0)==-EFAULT,
              "poll rejects inaccessible revents copyout");
        tracee_do_syscall(children[0],SYS_munmap,readonly_poll,4096,0,0,0,0);
        const pollfd fault_entry{-1,0,0x7f7f};
        auto* native_pages=static_cast<char*>(mmap(nullptr,8192,PROT_READ|PROT_WRITE,
            MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        if (native_pages==MAP_FAILED) throw std::runtime_error("native poll fault mmap");
        memcpy(native_pages+4096-7,&fault_entry,sizeof fault_entry);
        if(mprotect(native_pages+4096,4096,PROT_READ)) throw std::runtime_error("native poll fault mprotect");
        errno=0;
        check(syscall(SYS_poll,native_pages+4096-7,1,0)==-1 && errno==EFAULT &&
              native_pages[4095]==0x7f && native_pages[4096]==0x7f,
              "native poll cross-page revents store is atomic on fault");
        munmap(native_pages,8192);
        const long guest_pages=tracee_do_syscall(children[0],SYS_mmap,0,8192,
            PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(guest_pages<0) throw std::runtime_error("guest poll fault mmap");
        copy_to(children[0],reinterpret_cast<void*>(guest_pages+4096-7),&fault_entry,sizeof fault_entry);
        tracee_do_syscall(children[0],SYS_mprotect,guest_pages+4096,4096,PROT_READ,0,0,0);
        unsigned char scalar_bytes[2]{};
        check(emu_poll(reinterpret_cast<pollfd*>(guest_pages+4096-7),1,0)==-EFAULT &&
              guest_read_exact(children[0],reinterpret_cast<void*>(guest_pages+4095),scalar_bytes,2) &&
              scalar_bytes[0]==0x7f && scalar_bytes[1]==0x7f,
              "poll cross-page revents fault preserves both bytes like native");
        tracee_do_syscall(children[0],SYS_mprotect,guest_pages+4096,4096,PROT_READ|PROT_WRITE,0,0,0);
        check(emu_poll(reinterpret_cast<pollfd*>(guest_pages+4096-7),1,0)==0 &&
              guest_read_exact(children[0],reinterpret_cast<void*>(guest_pages+4095),scalar_bytes,2) &&
              scalar_bytes[0]==0 && scalar_bytes[1]==0,
              "poll cross-page writable revents succeeds");
        tracee_do_syscall(children[0],SYS_munmap,guest_pages,8192,0,0,0,0);
        int unconnected=ptmc_state.sock_states[0].do_socket(AF_INET,SOCK_STREAM,0);
        pollfd disconnected{unconnected,0,123};copy_to(children[0],guest_pollfds,&disconnected,sizeof disconnected);
        check(emu_poll(guest_pollfds,1,0)==1 &&
              guest_read_exact(children[0],guest_pollfds,&disconnected,sizeof disconnected) &&
              disconnected.revents==POLLHUP,"poll unconnected TCP hangup");emu_close(unconnected);
        bool blocking_poll_rejected=false;
        try {emu_poll(nullptr,0,-1);} catch(const std::runtime_error&) {blocking_poll_rejected=true;}
        check(blocking_poll_rejected,"blocking poll explicitly rejected");
        int poll_pipe[2];check(fs.do_pipe(poll_pipe,0)==0,"poll pipe created");
        pollfd ends[2]{{poll_pipe[0],0,123},{poll_pipe[1],POLLOUT,123}};
        copy_to(children[0],guest_pollfds,ends,sizeof ends);
        check(emu_poll(guest_pollfds,2,0)==1,"poll pipe writable");
        check(emu_close(poll_pipe[1])==0,"poll writer closed");
        copy_to(children[0],guest_pollfds,ends,sizeof ends);
        check(emu_poll(guest_pollfds,1,0)==1 &&
              guest_read_exact(children[0],guest_pollfds,ends,sizeof ends) && ends[0].revents==POLLHUP,
              "poll HUP reported without requested events");
        ends[0].events=POLLIN|POLLRDNORM;copy_to(children[0],guest_pollfds,ends,sizeof ends);
        check(emu_poll(guest_pollfds,1,0)==1 &&
              guest_read_exact(children[0],guest_pollfds,ends,sizeof ends) && ends[0].revents==POLLHUP,
              "poll empty pipe EOF is HUP without readable data");
        emu_close(poll_pipe[0]);
        int native_pipe[2];check(pipe(native_pipe)==0,"native poll oracle pipe created");
        close(native_pipe[0]);pollfd native_end{native_pipe[1],POLLOUT,0};
        check(poll(&native_end,1,0)==1 && native_end.revents==(POLLOUT|POLLERR),
              "native poll closed-reader oracle");close(native_pipe[1]);
        fs.do_pipe(poll_pipe,0);emu_close(poll_pipe[0]);
        pollfd broken{poll_pipe[1],POLLOUT,0};copy_to(children[0],guest_pollfds,&broken,sizeof broken);
        check(emu_poll(guest_pollfds,1,0)==1 &&
              guest_read_exact(children[0],guest_pollfds,&broken,sizeof broken) && broken.revents==(POLLOUT|POLLERR),
              "poll closed-reader matches native oracle");emu_close(poll_pipe[1]);
        bool unsupported_rejected=true;
        for (int number:{SYS_dup,SYS_readv,SYS_unlink,SYS_fchdir,SYS_flock,SYS_sync,SYS_syncfs,SYS_readahead,SYS_fadvise64}) {
            syscall_info rejected{};rejected.nr=number;
            bool threw=false;
            try {dispatcher.on_enter(children[0],number,rejected);}
            catch (const std::runtime_error&) {threw=true;}
            unsupported_rejected=unsupported_rejected && threw &&
                dispatcher.get_route(number).action==SyscallAction::REJECT;
        }
        check(unsupported_rejected,"unsupported descriptor and FS operations rejected before kernel");
        int real_fd;
        iovec fd_local{&real_fd,sizeof(real_fd)},fd_remote{&guest_real_fd,sizeof(real_fd)};
        if(process_vm_readv(children[0],&fd_local,1,&fd_remote,1,0)!=sizeof(real_fd)) throw std::runtime_error("real fd");
        const auto physical_flags=tracee_do_syscall(children[0],SYS_fcntl,real_fd,F_GETFD,0,0,0,0);
        check(execute(dispatcher,children[0],SYS_fcntl,{uint64_t(file),F_SETFD,FD_CLOEXEC,0,0,0})==0 &&
              emu_fcntl(file,F_GETFD,0)==FD_CLOEXEC &&
              tracee_do_syscall(children[0],SYS_fcntl,real_fd,F_GETFD,0,0,0,0)==physical_flags,
              "actual entry blocks host descriptor mutation");
        check(execute(dispatcher,children[0],SYS_pipe,{reinterpret_cast<uint64_t>(guest_pipefds),0xdeadbeef,0,0,0,0})==0,
              "pipe ignores nonexistent second argument");
        const int mapped=fs.do_open("/mapped",O_CREAT|O_RDWR,0600);
        fs.do_write(mapped,"mapping-data",12);
        user_regs_struct map_registers;
        check(execute(dispatcher,children[0],SYS_mmap,{1,4096,PROT_READ,MAP_PRIVATE|MAP_FIXED_NOREPLACE,uint64_t(mapped),0})==-EINVAL,
              "virtual mmap preserves kernel failure without attempting copyout");
        const long mapping=execute(dispatcher,children[0],SYS_mmap,{0,4096,PROT_READ,MAP_PRIVATE,uint64_t(mapped),0},&map_registers);
        char mapped_bytes[12]{};
        iovec map_local{mapped_bytes,sizeof(mapped_bytes)},map_remote{reinterpret_cast<void*>(mapping),sizeof(mapped_bytes)};
        check(mapping>0 && process_vm_readv(children[0],&map_local,1,&map_remote,1,0)==12 &&
              !memcmp(mapped_bytes,"mapping-data",12) && map_registers.r8==uint64_t(mapped) &&
              map_registers.rdx==PROT_READ && map_registers.r10==MAP_PRIVATE,
              "virtual mmap preserves contents and syscall arguments");
        unsigned char replacement=1;
        iovec change_local{&replacement,1},change_remote{reinterpret_cast<void*>(mapping),1};
        check(process_vm_writev(children[0],&change_local,1,&change_remote,1,0)==-1,
              "virtual mmap restores read-only protection");
        check(mapping_entry_rejected(dispatcher,children[0],SYS_madvise,mapping,4096,MADV_DONTNEED),
              "virtual file mapping cannot be discarded as host-anonymous memory");
        check(mapping_entry_rejected(dispatcher,children[0],SYS_mprotect,mapping,4096,PROT_NONE),
              "virtual file mapping cannot become an anonymous guard");
        check(process_vm_readv(children[0],&map_local,1,&map_remote,1,0)==12 &&
              !memcmp(mapped_bytes,"mapping-data",12),"rejected virtual mapping mutation preserves bytes");
        execute(dispatcher,children[0],SYS_munmap,{uint64_t(mapping),4096,0,0,0,0});
        // Keep the logical backing provenance despite the anonymous host VMA.
        // Exercise page-rounded holes and replacement mappings through real
        // syscall entry/exit, not direct interval bookkeeping alone.
        std::vector<char> file_pages(3*4096,'v');
        check(fs.do_lseek(mapped,0,SEEK_SET)==0 &&
              fs.do_write(mapped,file_pages.data(),file_pages.size())==long(file_pages.size()),
              "prepare three-page virtual file");
        const long range=execute(dispatcher,children[0],SYS_mmap,
            {0,3*4096,PROT_READ|PROT_WRITE,MAP_PRIVATE,uint64_t(mapped),0});
        check(range>0 && fs.virtual_file_mappings==std::map<uint64_t,uint64_t>{{uint64_t(range),uint64_t(range)+3*4096}},
              "virtual mapping provenance records exact rounded interval");
        const auto mapped_archive=archive(fs);
        FileSystemState copied_fs=fs;
        copied_fs.remove_virtual_file_mapping(range,range+4096);
        check(fs.overlaps_virtual_file_mapping(range,range+4096) &&
              !copied_fs.overlaps_virtual_file_mapping(range,range+4096) && archive(copied_fs)!=mapped_archive,
              "virtual mapping provenance is branch-isolated semantic state");
        FileSystemState restored_fs;
        std::istringstream mapping_input(mapped_archive);
        {cereal::BinaryInputArchive input(mapping_input);input(restored_fs);}
        restored_fs.set_fd_manager(ptmc_state.fd_managers[0]);
        fs=restored_fs;
        check(archive(fs)==mapped_archive &&
              mapping_entry_rejected(dispatcher,children[0],SYS_madvise,range,4096,MADV_DONTNEED) &&
              mapping_entry_rejected(dispatcher,children[0],SYS_mprotect,range,4096,PROT_NONE),
              "deserialized mapping provenance restores both admission restrictions");
        check(execute(dispatcher,children[0],SYS_mmap,
                  {uint64_t(range),4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,uint64_t(-1),0})==-EEXIST &&
              archive(fs)==mapped_archive,"failed anonymous replacement preserves virtual mapping provenance");
        check(execute(dispatcher,children[0],SYS_mmap,
                  {uint64_t(range)+1,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,uint64_t(-1),0})==-EINVAL &&
              archive(fs)==mapped_archive,"failed fixed mmap preserves virtual mapping provenance");
        check(execute(dispatcher,children[0],SYS_munmap,{uint64_t(range)+1,4096,0,0,0,0})==-EINVAL &&
              archive(fs)==mapped_archive,"failed munmap preserves virtual mapping provenance");
        check(execute(dispatcher,children[0],SYS_munmap,{uint64_t(range)+4096,1,0,0,0,0})==0 &&
              fs.virtual_file_mappings==std::map<uint64_t,uint64_t>{
                  {uint64_t(range),uint64_t(range)+4096},
                  {uint64_t(range)+8192,uint64_t(range)+12288}},
              "partial munmap splits virtual provenance with kernel page rounding");
        check(execute(dispatcher,children[0],SYS_mmap,
                  {uint64_t(range),1,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,uint64_t(-1),0})==range &&
              !fs.overlaps_virtual_file_mapping(range,range+4096) &&
              fs.overlaps_virtual_file_mapping(range+8192,range+12288),
              "successful anonymous fixed mmap removes only replaced provenance");
        const char dirty='d';copy_to(children[0],reinterpret_cast<void*>(range),&dirty,1);
        check(execute(dispatcher,children[0],SYS_madvise,{uint64_t(range),4096,MADV_DONTNEED,0,0,0})==0,
              "genuinely anonymous replacement accepts discard");
        char zero=1;
        check(guest_read_exact(children[0],reinterpret_cast<void*>(range),&zero,1) && zero==0,
              "anonymous discard uses kernel zero-fill semantics");
        check(execute(dispatcher,children[0],SYS_mprotect,{uint64_t(range),4096,PROT_NONE,0,0,0})==0 &&
              execute(dispatcher,children[0],SYS_mprotect,{uint64_t(range),4096,PROT_READ|PROT_WRITE,0,0,0})==0,
              "genuinely anonymous replacement accepts guard protection");
        check(execute(dispatcher,children[0],SYS_mmap,
                  {uint64_t(range)+4096,4096,PROT_READ,MAP_PRIVATE|MAP_FIXED,uint64_t(mapped),4096})==range+4096 &&
              fs.virtual_file_mappings==std::map<uint64_t,uint64_t>{{uint64_t(range)+4096,uint64_t(range)+12288}},
              "virtual fixed mmap restores and canonically merges backing provenance");
        check(fs.do_close(mapped)==0 &&
              mapping_entry_rejected(dispatcher,children[0],SYS_madvise,range+4096,4096,MADV_DONTNEED),
              "closing virtual descriptor does not lose mapping provenance");
        check(execute(dispatcher,children[0],SYS_munmap,{uint64_t(range),12288,0,0,0,0})==0 &&
              fs.virtual_file_mappings.empty(),"complete munmap removes remaining backing provenance");
        // brk shrink removes VMAs even if a MAP_FIXED virtual-file mapping
        // replaced part of the grown heap. Snapshot capture normally refreshes
        // child.brk between transitions; this dispatcher fixture does so explicitly.
        const uint64_t old_break=execute(dispatcher,children[0],SYS_brk,{0,0,0,0,0,0});
        const uint64_t heap_page=(old_break+4095)&~uint64_t(4095);
        const uint64_t grown_break=heap_page+8192;
        ptmc_state.running_state.child[0].brk=old_break;
        check(uint64_t(execute(dispatcher,children[0],SYS_brk,{grown_break,0,0,0,0,0}))==grown_break,
              "grow heap for fixed virtual mapping");
        ptmc_state.running_state.child[0].brk=grown_break;
        const int heap_file=fs.do_open("/mapped",O_RDONLY,0);
        check(execute(dispatcher,children[0],SYS_mmap,
                  {heap_page+4096,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_FIXED,uint64_t(heap_file),0})==long(heap_page+4096) &&
              fs.overlaps_virtual_file_mapping(heap_page+4096,grown_break),
              "virtual fixed mapping inside grown heap records provenance");
        const auto heap_archive=archive(fs);
        check(uint64_t(execute(dispatcher,children[0],SYS_brk,{0,0,0,0,0,0}))==grown_break &&
              archive(fs)==heap_archive,"query brk preserves virtual provenance");
        check(uint64_t(execute(dispatcher,children[0],SYS_brk,{1,0,0,0,0,0}))==grown_break &&
              archive(fs)==heap_archive,"failed brk preserves virtual provenance");
        check(uint64_t(execute(dispatcher,children[0],SYS_brk,{old_break,0,0,0,0,0}))==old_break &&
              fs.virtual_file_mappings.empty(),"brk shrink removes virtual provenance with removed heap pages");
        ptmc_state.running_state.child[0].brk=old_break;
        check(uint64_t(execute(dispatcher,children[0],SYS_brk,{grown_break,0,0,0,0,0}))==grown_break &&
              !fs.overlaps_virtual_file_mapping(heap_page+4096,grown_break),
              "regrown heap has no stale virtual backing provenance");
        ptmc_state.running_state.child[0].brk=grown_break;
        check(uint64_t(execute(dispatcher,children[0],SYS_brk,{old_break,0,0,0,0,0}))==old_break,
              "restore original heap extent");
        ptmc_state.running_state.child[0].brk=old_break;
        fs.do_close(heap_file);
        fs.ensure_dir_exists("/");fs.ensure_dir_exists("/dir");
        const int directory=fs.do_open("/dir",O_RDONLY|O_DIRECTORY,0);
        const char relative[]="child";
        copy_to(children[0],guest_bytes,relative,sizeof(relative));
        const int child_file=emu_vfs_openat(directory,guest_bytes,O_CREAT|O_RDWR,0600);
        check(child_file>=0 && fs.open_files.at(child_file).path=="/dir/child",
              "openat resolves relative to directory descriptor");
        check(emu_vfs_openat(999,guest_bytes,O_RDONLY,0)==-EBADF &&
              emu_vfs_openat(child_file,guest_bytes,O_RDONLY,0)==-ENOTDIR,
              "openat rejects absent and non-directory dirfd");
        check(emu_vfs_openat(directory,reinterpret_cast<const char*>(1),O_RDONLY,0)==-EFAULT,
              "openat checks guest pathname copy");
        check(emu_vfs_statat(directory,guest_bytes,&guest_stat,0)==0,
              "statat resolves directory-relative path");
        const char empty[]="";
        copy_to(children[0],guest_bytes,empty,sizeof(empty));
        check(emu_vfs_statat(child_file,guest_bytes,&guest_stat,AT_EMPTY_PATH)==0 &&
              emu_vfs_statat(directory,guest_bytes,&guest_stat,0)==-ENOENT,
              "statat empty pathname requires AT_EMPTY_PATH");
        check(emu_vfs_statat(child_file,guest_bytes,reinterpret_cast<struct stat*>(1),AT_EMPTY_PATH)==-EFAULT &&
              emu_vfs_statat(child_file,guest_bytes,&guest_stat,0x40000000)==-EINVAL,
              "statat validates copyout and flags");
        fs.set_cwd("/dir");
        check(emu_getcwd(guest_bytes,sizeof(guest_bytes))==5,
              "getcwd returns NUL-inclusive length");
        char cwd[5]{};
        check(guest_read_exact(children[0],guest_bytes,cwd,sizeof(cwd)) && !strcmp(cwd,"/dir") &&
              emu_getcwd(guest_bytes,4)==-ERANGE && emu_getcwd(guest_bytes,0)==-EINVAL &&
              emu_getcwd(reinterpret_cast<char*>(1),5)==-EFAULT,
              "getcwd validates size and guest copy");
        copy_to(children[0],guest_bytes,"abc",4);
        int pipefd[2]; check(fs.do_pipe(pipefd,O_NONBLOCK)==0,"pipe created");
        check(emu_write(pipefd[1],guest_bytes,3)==3 && emu_read(pipefd[0],guest_bytes,3)==3,"pipe read/write dispatched");
        fs.do_pipe_write(pipefd[1],guest_bytes,3);
        auto encoded=archive(fs);FileSystemState restored;{std::istringstream input(encoded);cereal::BinaryInputArchive a(input);a(restored);}
        check(restored.pipe_fds_.size()==fs.pipe_fds_.size() && !restored.pipes_.empty(),"pipe state archived");
        auto pair=connected();
        ptmc_state.cursor=1;check(emu_write(pair.first,guest_bytes,0)==0,"zero TCP write succeeds");
        ptmc_state.cursor=0;check(emu_read(pair.second,guest_bytes,8)==-EAGAIN,"zero TCP write is not EOF");
        ptmc_state.cursor=1;emu_write(pair.first,guest_bytes,3);
        ptmc_state.cursor=0;auto before=archive(ptmc_state.sock_states[0]);
        check(emu_read(pair.second,reinterpret_cast<void*>(1),3)==-EFAULT &&
              archive(ptmc_state.sock_states[0])==before,"bad TCP copyout preserves queue");
        emu_read(pair.second,guest_bytes,3);
        ptmc_state.cursor=1;emu_close(pair.first);ptmc_state.cursor=0;
        check(emu_read(pair.second,guest_bytes,3)==0 && emu_read(pair.second,guest_bytes,3)==0,"TCP EOF persists");
        pair=connected(false);ptmc_state.cursor=1;emu_write(pair.first,guest_bytes,3);ptmc_state.cursor=0;
        encoded=archive(ptmc_state.sock_states[0]);SockState restored_net;
        {std::istringstream input(encoded);cereal::BinaryInputArchive a(input);a(restored_net);}
        check(!restored_net.get_socket(pair.second)->established_connections.empty(),"pre-accept TCP payload archived");
        reset();socket=ptmc_state.sock_states[0].do_socket(AF_INET,SOCK_DGRAM,0);
        check(emu_write(socket,guest_bytes,3)==-EDESTADDRREQ,
              "UDP common write requires destination for unconnected socket");
        UdpDatagram common_datagram{};common_datagram.content="udp";
        ptmc_state.sock_states[0].udp_recv_buffers()[socket].push_back(common_datagram);
        const auto udp_before=archive(ptmc_state.sock_states[0]);
        check(emu_read(socket,reinterpret_cast<void*>(1),3)==-EFAULT &&
              archive(ptmc_state.sock_states[0])==udp_before,
              "UDP common read bad copyout preserves datagram");
        check(emu_ioctl(socket,FIONREAD,&guest_integer)==0,
              "UDP FIONREAD copies first datagram size");
        int pending=0;guest_read_exact(children[0],&guest_integer,&pending,sizeof(pending));
        char udp_bytes[3]{};
        const auto udp_read=emu_read(socket,guest_bytes,3);
        check(udp_read==3 && guest_read_exact(children[0],guest_bytes,udp_bytes,sizeof(udp_bytes)) &&
              !memcmp(udp_bytes,"udp",3) && pending==3 && emu_read(socket,guest_bytes,3)==-EAGAIN,
              "UDP common read consumes datagram payload");
        int nonblocking=1;
        copy_to(children[0],&guest_integer,&nonblocking,sizeof(nonblocking));
        check(emu_ioctl(socket,FIONBIO,&guest_integer)==0 &&
              (emu_fcntl(socket,F_GETFL,0)&O_NONBLOCK) &&
              emu_ioctl(socket,FIONBIO,reinterpret_cast<void*>(1))==-EFAULT,
              "FIONBIO updates virtual flags with checked guest copy");
        check(emu_fstat(socket,&guest_stat)==0 &&
              guest_read_exact(children[0],&guest_stat,&guest_stat,sizeof(guest_stat)) &&
              S_ISSOCK(guest_stat.st_mode) && emu_fstat(socket,reinterpret_cast<struct stat*>(1))==-EFAULT,
              "fstat reports socket kind and rejects invalid copyout");
        int epoll=emu_epoll_create(4);epoll_event event{};event.events=EPOLLIN;event.data.u64=0x123456789abcdef0ULL;
        copy_to(children[0],&guest_event,&event,sizeof(event));
        emu_epoll_ctl(epoll,EPOLL_CTL_ADD,socket,&guest_event);
        check(emu_epoll_ctl(epoll,EPOLL_CTL_ADD,socket,&guest_event)==-EEXIST,"duplicate epoll ADD rejected");
        UdpDatagram datagram{};datagram.content="x";ptmc_state.sock_states[0].udp_recv_buffers()[socket].push_back(datagram);
        pollfd epoll_probe{epoll,POLLIN,0};copy_to(children[0],guest_pollfds,&epoll_probe,sizeof epoll_probe);
        const auto poll_net_before=archive(ptmc_state.sock_states[0]);
        check(emu_poll(guest_pollfds,1,0)==1 &&
              guest_read_exact(children[0],guest_pollfds,&epoll_probe,sizeof epoll_probe) &&
              epoll_probe.revents==POLLIN && archive(ptmc_state.sock_states[0])==poll_net_before,
              "poll epoll readiness does not consume events");
        int native_poll_pipe[2];
        check(pipe(native_poll_pipe)==0,"native eventpoll oracle pipe created");
        int native_epoll=epoll_create1(0);epoll_event native_watch{};native_watch.events=EPOLLIN;
        check(native_epoll>=0 && epoll_ctl(native_epoll,EPOLL_CTL_ADD,native_poll_pipe[0],&native_watch)==0 &&
              write(native_poll_pipe[1],"x",1)==1,"native eventpoll oracle ready");
        pollfd native_epoll_probe{native_epoll,POLLRDNORM,0};
        check(poll(&native_epoll_probe,1,0)==1 && native_epoll_probe.revents==POLLRDNORM,
              "native poll epoll POLLRDNORM oracle");
        close(native_epoll);close(native_poll_pipe[0]);close(native_poll_pipe[1]);
        epoll_probe.events=POLLRDNORM;epoll_probe.revents=0;
        copy_to(children[0],guest_pollfds,&epoll_probe,sizeof epoll_probe);
        check(emu_poll(guest_pollfds,1,0)==1 &&
              guest_read_exact(children[0],guest_pollfds,&epoll_probe,sizeof epoll_probe) &&
              epoll_probe.revents==POLLRDNORM && archive(ptmc_state.sock_states[0])==poll_net_before,
              "poll epoll POLLRDNORM matches native oracle without consuming events");
        auto ready=emu_epoll_wait(epoll,guest_events,1,0);
        epoll_event received{};iovec local{&received,sizeof(received)},remote{guest_events,sizeof(received)};
        process_vm_readv(children[0],&local,1,&remote,1,0);
        check(ready==1 && received.data.u64==event.data.u64,"epoll opaque userdata preserved");
        emu_close(socket);
        check(!ptmc_state.sock_states[0].epoll_instances().at(epoll).watched_fds.count(socket),"close removes epoll watches");
        check(emu_close(epoll)==0 && !ptmc_state.fd_managers[0]->is_allocated(epoll),"epoll close releases descriptor");
        void* sentinel=mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(sentinel==MAP_FAILED) throw std::runtime_error("sentinel");
        syscall_info info{};info.nr=SYS_munmap;info.args[0]=reinterpret_cast<uint64_t>(sentinel);info.args[1]=4096;info.rval=0;
        handlers::fs::handle_munmap(children[0],info);
        unsigned char resident;
        check(mincore(sentinel,4096,&resident)==0,"guest munmap never unmaps tracer memory");
        munmap(sentinel,4096);
        for(auto child:children) {kill(child,SIGKILL);waitpid(child,nullptr,0);}
        return failures?1:0;
    } catch(const std::exception& error) {
        for(auto child:children) if(child>0) {kill(child,SIGKILL);waitpid(child,nullptr,0);}
        fprintf(stderr,"io boundary: %s\n",error.what());return 1;
    }
}
