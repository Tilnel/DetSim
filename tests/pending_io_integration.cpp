#include "monitor.h"
#include "ncurses_ui.h"
#include "guest.h"
#include "io.h"
#include "pending_io.h"
#include "engine/pending_syscall.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>

static_assert(NP == 2);
extern "C" detsim::ui::NCursesUI* get_ncurses_ui() { return nullptr; }
static char guest_data[65536] = "payload";
static int pipefds[2];
static sockaddr_in address;
static epoll_event event, events[4];
static timespec guest_timeout;
static void check(bool ok, const char* label) {
  if (!ok) throw std::runtime_error(label);
  printf("PASS %s\n", label);
}
static syscall_info call(int nr, int fd, uintptr_t buffer = 0, size_t count = 0) {
  syscall_info value{}; value.nr = nr; value.args[0] = fd;
  value.args[1] = buffer; value.args[2] = count; return value;
}
static void set_retry(int node) {
  auto& pending = *ptmc_state.running_state.child[node].threads[0].pending_syscall;
  pending.phase = PendingPhase::Retry;
  sync_pending_blocked(node);
}
int main() {
  pid_t children[NP]{};
  try {
    for (int node=0; node<NP; ++node) {
      children[node] = fork();
      if (children[node] < 0) throw std::runtime_error("fork");
      if (!children[node]) {
        if (ptrace(PTRACE_TRACEME,0,nullptr,nullptr)) _exit(2);
        asm volatile("int3"); _exit(0);
      }
      int status;
      if (waitpid(children[node],&status,0)!=children[node] || !WIFSTOPPED(status))
        throw std::runtime_error("stop");
      ptmc_state.pids[node] = children[node];
      auto& state = ptmc_state.running_state.child[node];
      state.add_thread(children[node],0,0,0,true);
      ptmc_state.fd_managers[node] = std::make_shared<FdManager>();
      ptmc_state.fs_states[node].set_fd_manager(ptmc_state.fd_managers[node]);
      ptmc_state.sock_states[node].set_fd_manager(ptmc_state.fd_managers[node]);
      ptmc_state.addrs[node] = node ? "192.168.0.2" : "192.168.0.1";
    }
    ptmc_state.cursor=0;
    auto& fs=ptmc_state.fs_states[0]; auto& net=ptmc_state.sock_states[0];
    check(fs.do_pipe(pipefds,0)==0,"create pipe");
    auto read=call(SYS_read,pipefds[0],uintptr_t(guest_data),7);
    check(io_should_suspend(0,read,emu_read(pipefds[0],guest_data,7)) && !io_retry_ready(0,read),"empty blocking read suspends");
    suspend_syscall(0,children[0],read,PendingReason::Io);
    bool rejected=false;
    try { emu_close(pipefds[0]); } catch(const std::runtime_error&) { rejected=true; }
    check(rejected,"pending descriptor close rejected");
    rejected=false;
    try { emu_fcntl(pipefds[0],F_SETFL,O_NONBLOCK); } catch(const std::runtime_error&) { rejected=true; }
    check(rejected,"pending descriptor flag mutation rejected");
    check(emu_write(pipefds[1],guest_data,7)==7 && io_retry_ready(0,read),"pipe write enables retry");
    set_retry(0); check(complete_pending_syscall(0,children[0],read) && read.rval==7,"read completion consumes queued data");
    check(!ptmc_state.running_state.child[0].threads[0].pending_syscall,"completion clears pending record");
    suspend_syscall(0,children[0],read,PendingReason::Io);
    check(emu_write(pipefds[1],guest_data,7)==7,"retry-race payload queued");
    set_retry(0);
    check(emu_read(pipefds[0],guest_data,7)==7,"competing reader consumes readiness");
    check(complete_pending_syscall(0,children[0],read) &&
          ptmc_state.running_state.child[0].threads[0].pending_syscall->phase==PendingPhase::Waiting &&
          long(read.rval)==-ENOSYS,"lost readiness rearms wait without fake result");
    check(emu_write(pipefds[1],guest_data,7)==7,"replacement payload queued");
    set_retry(0);check(complete_pending_syscall(0,children[0],read) && read.rval==7,"rearmed read completes once");

    check(emu_write(pipefds[1],guest_data,sizeof(guest_data))==sizeof(guest_data),"pipe fills fixed capacity");
    rejected=false;
    try { emu_write(pipefds[1],guest_data,8192); } catch(const std::runtime_error&) { rejected=true; }
    check(rejected && fs.pipes_.begin()->second.buffer.size()==sizeof(guest_data),"partial blocking write rejected without mutation");
    auto write=call(SYS_write,pipefds[1],uintptr_t(guest_data),1);
    check(io_should_suspend(0,write,emu_write(pipefds[1],guest_data,1)) && !io_retry_ready(0,write),"full pipe write blocks");
    check(emu_read(pipefds[0],guest_data,1)==1 && io_retry_ready(0,write),"read makes pipe writer ready");
    write.args[2]=4096;
    check(emu_write(pipefds[1],guest_data,4096)==-EAGAIN && !io_retry_ready(0,write),"PIPE_BUF write waits atomically");
    check(emu_fcntl(pipefds[1],F_SETFL,O_NONBLOCK)==0 && !io_should_suspend(0,write,-EAGAIN),"nonblocking pipe preserves EAGAIN");
    check(emu_read(pipefds[0],guest_data,sizeof(guest_data))==sizeof(guest_data)-1,"drain pipe");
    check(emu_close(pipefds[1])==0 && io_retry_ready(0,read) && emu_read(pipefds[0],guest_data,7)==0,"writer close enables persistent EOF");

    const int receiver=net.do_socket(AF_INET,SOCK_DGRAM,0);
    sockaddr_in udp_address{};udp_address.sin_family=AF_INET;udp_address.sin_port=htons(9001);
    inet_pton(AF_INET,"192.168.0.1",&udp_address.sin_addr);
    check(guest_write_exact(children[0],&address,&udp_address,sizeof(udp_address)) &&
          net.do_bind(receiver,reinterpret_cast<sockaddr*>(&address),sizeof(udp_address))==0,"bind pending UDP receiver");
    auto datagram=call(SYS_recvfrom,receiver,uintptr_t(guest_data),7);
    check(io_should_suspend(0,datagram,net.do_recvfrom(receiver,guest_data,7,0,nullptr,nullptr)) &&
          !io_retry_ready(0,datagram),"empty blocking UDP receive waits");
    suspend_syscall(0,children[0],datagram,PendingReason::Io);
    ptmc_state.cursor=1;
    const int sender=ptmc_state.sock_states[1].do_socket(AF_INET,SOCK_DGRAM,0);
    check(guest_write_exact(children[1],&address,&udp_address,sizeof(udp_address)) &&
          ptmc_state.sock_states[1].do_sendto(sender,guest_data,7,0,
              reinterpret_cast<sockaddr*>(&address),sizeof(udp_address))==7,"deliver cross-node UDP payload");
    ptmc_state.cursor=0;
    refresh_pending_syscalls();
    check(ptmc_state.running_state.child[0].threads[0].pending_syscall->phase==PendingPhase::Retry &&
          complete_pending_syscall(0,children[0],datagram) && datagram.rval==7 &&
          net.get_udp_buffer_size(receiver)==0,"pending UDP completion consumes exactly one datagram");

    const int listener=net.do_socket(AF_INET,SOCK_STREAM,0);
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(9000);
    inet_pton(AF_INET,"192.168.0.1",&addr.sin_addr);
    check(guest_write_exact(children[0],&address,&addr,sizeof(addr)),"copy listener address");
    check(net.do_bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0 && net.do_listen(listener,8)==0,"listen");
    auto accept=call(SYS_accept4,listener);accept.args[3]=SOCK_NONBLOCK|SOCK_CLOEXEC;
    check(io_should_suspend(0,accept,emu_accept4(listener,nullptr,nullptr,accept.args[3])) && !io_retry_ready(0,accept),"accept4 flags do not make listener nonblocking");
    suspend_syscall(0,children[0],accept,PendingReason::Io);
    ptmc_state.cursor=1;
    const int client=ptmc_state.sock_states[1].do_socket(AF_INET,SOCK_STREAM,0);
    check(guest_write_exact(children[1],&address,&addr,sizeof(addr)),"copy connect address");
    check(ptmc_state.sock_states[1].do_connect(client,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"cross-node connect");
    ptmc_state.cursor=0;
    const auto before_accept_fds=ptmc_state.fd_managers[0]->get_allocated_fds();
    rejected=false;
    try {preflight_process_io_exit(0);} catch(const std::runtime_error&) {rejected=true;}
    check(rejected && ptmc_state.fd_managers[0]->get_allocated_fds()==before_accept_fds &&
          net.get_socket(listener)->pending_connections.size()==1 &&
          ptmc_state.running_state.child[0].threads[0].alive &&
          ptmc_state.running_state.child[0].threads[0].pending_syscall.has_value(),
          "exit rejects unaccepted connection before FD, queue or thread mutation");
    check(io_retry_ready(0,accept),"connect enables accept retry");set_retry(0);
    check(complete_pending_syscall(0,children[0],accept) && long(accept.rval)>=0,"accept completes");
    const int server=accept.rval;
    check((emu_fcntl(server,F_GETFL,0)&O_NONBLOCK) && emu_fcntl(server,F_GETFD,0)==FD_CLOEXEC,"accept4 installs new descriptor flags");
    emu_fcntl(server,F_SETFL,0);
    auto receive=call(SYS_recvfrom,server,uintptr_t(guest_data),7);
    check(io_should_suspend(0,receive,net.do_recv(server,guest_data,7,0)),"empty TCP receive blocks");
    receive.args[3]=MSG_DONTWAIT;check(!io_should_suspend(0,receive,-EAGAIN),"MSG_DONTWAIT preserves EAGAIN");receive.args[3]=0;
    const int epfd=emu_epoll_create1(0);
    epoll_event watch{};watch.events=EPOLLIN;watch.data.u64=0x12345678;
    check(guest_write_exact(children[0],&event,&watch,sizeof(watch)) && emu_epoll_ctl(epfd,EPOLL_CTL_ADD,server,&event)==0,"register epoll read watch");
    auto wait=call(SYS_epoll_wait,epfd,uintptr_t(events),4);wait.args[3]=uint64_t(-1);
    check(io_should_suspend(0,wait,emu_epoll_wait(epfd,events,4,-1)) && !io_retry_ready(0,wait),"indefinite epoll blocks");
    wait.args[3]=0;check(!io_should_suspend(0,wait,0),"zero-time epoll polls");
    wait.args[3]=1;check(io_should_suspend(0,wait,0),"finite epoll remains pending");
    suspend_syscall(0,children[0],wait,PendingReason::Io);
    configure_pending_io(0,children[0],wait);
    const auto logical_tid=ptmc_state.running_state.child[0].threads[0].tid;
    check(!pending_timeout_due(0,logical_tid) && !expire_pending_syscall(0,logical_tid),"epoll timeout cannot expire early");
    ptmc_state.time[0].tv_usec=1000;
    check(pending_timeout_due(0,logical_tid) && expire_pending_syscall(0,logical_tid),"epoll deadline permits explicit expiry");
    check(complete_pending_syscall(0,children[0],wait) && wait.rval==0,"epoll timeout completes with zero events");
    ptmc_state.time[0]={};
    auto wait2=call(SYS_epoll_pwait2,epfd,uintptr_t(events),4);
    wait2.args[3]=uintptr_t(&guest_timeout);
    timespec tiny_timeout{0,17};
    check(guest_write_exact(children[0],&guest_timeout,&tiny_timeout,sizeof(tiny_timeout)),"copy nanosecond epoll timeout");
    check(io_should_suspend(0,wait2,emu_epoll_pwait2(epfd,events,4,&guest_timeout,nullptr,8)),"pwait2 finite wait suspends");
    suspend_syscall(0,children[0],wait2,PendingReason::Io);
    configure_pending_io(0,children[0],wait2);
    check(ptmc_state.running_state.child[0].threads[0].pending_syscall->deadline->nsec==17,"pwait2 retains exact nanoseconds");
    tiny_timeout={100,0};
    check(guest_write_exact(children[0],&guest_timeout,&tiny_timeout,sizeof(tiny_timeout)),"mutate original timeout after registration");
    set_retry(0);
    check(complete_pending_syscall(0,children[0],wait2) &&
          ptmc_state.running_state.child[0].threads[0].pending_syscall->deadline->nsec==17,"failed readiness retry preserves captured deadline");
    ptmc_state.time[0].tv_usec=1;
    check(expire_pending_syscall(0,logical_tid) && complete_pending_syscall(0,children[0],wait2) && wait2.rval==0,"pwait2 expiry ignores changed input memory");
    ptmc_state.time[0]={};
    wait.args[3]=uint64_t(-1);
    suspend_syscall(0,children[0],wait,PendingReason::Io);
    ptmc_state.cursor=1;check(emu_write(client,guest_data,7)==7,"cross-node TCP send");
    ptmc_state.cursor=0;check(io_retry_ready(0,wait) && io_retry_ready(0,receive),"TCP payload enables epoll and receive");
    const auto before_unread_fds=ptmc_state.fd_managers[0]->get_allocated_fds();
    rejected=false;
    try {preflight_process_io_exit(0);} catch(const std::runtime_error&) {rejected=true;}
    check(rejected && ptmc_state.fd_managers[0]->get_allocated_fds()==before_unread_fds &&
          net.tcp_connections().at(server).recv_buffer.front().size()==7 &&
          ptmc_state.running_state.child[0].threads[0].alive &&
          ptmc_state.running_state.child[0].threads[0].pending_syscall.has_value(),
          "exit rejects unread TCP payload without mutation");

    set_retry(0);check(complete_pending_syscall(0,children[0],wait) && wait.rval==1,"epoll completion reports readiness");
    epoll_event result{};check(guest_read_exact(children[0],events,&result,sizeof(result)) && result.data.u64==watch.data.u64,"epoll completion retains user data");
    check(net.do_recv(server,guest_data,7,0)==7,"TCP payload consumed");
    suspend_syscall(0,children[0],receive,PendingReason::Io);
    ptmc_state.cursor=1;
    preflight_process_io_exit(1);
    rejected=false;try {close_process_io(1);}catch(const std::runtime_error&){rejected=true;}
    check(rejected && ptmc_state.fd_managers[1]->is_allocated(client),"process I/O close requires all tasks exited");
    ptmc_state.running_state.child[1].threads[0].alive=false;
    ptmc_state.cursor=0;
    close_process_io(1);
    check(ptmc_state.cursor==0,"process I/O close restores caller cursor");
    check(ptmc_state.fd_managers[1]->get_allocated_fds().empty(),"normal process exit closes all descriptors");
    ptmc_state.cursor=0;
    check(io_retry_ready(0,receive),"peer close enables receive EOF");set_retry(0);
    check(complete_pending_syscall(0,children[0],receive) && receive.rval==0,"pending receive completes EOF");
    for(auto child:children){kill(child,SIGKILL);waitpid(child,nullptr,0);}return 0;
  } catch(const std::exception& error) {
    for(auto child:children)if(child>0){kill(child,SIGKILL);waitpid(child,nullptr,0);}
    fprintf(stderr,"pending IO: %s\n",error.what());return 1;
  }
}
