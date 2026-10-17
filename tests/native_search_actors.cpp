#include "state/state_transition.h"
#include "proc_status.h"
#include "cereal/archives/binary.hpp"
#include <cereal/types/map.hpp>
#include <cereal/types/deque.hpp>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <sys/syscall.h>
#include <time.h>
namespace detsim { namespace ui { class NCursesUI; } }
extern "C" detsim::ui::NCursesUI* get_ncurses_ui(){return nullptr;}
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static std::string serialized(const tracee_state& source){std::ostringstream out;cereal::BinaryOutputArchive archive(out);archive(source);return out.str();}
static PendingSyscall timer(int64_t nanos) {
    PendingSyscall call;call.nr=SYS_nanosleep;call.reason=PendingReason::Timer;call.args[0]=0x1000;
    call.deadline=PendingDeadline{CLOCK_MONOTONIC,1,static_cast<int32_t>(nanos),0};return call;
}
static void expect(const std::vector<ActorAction>& actual,std::initializer_list<ActorAction> expected) {
    require(actual.size()==expected.size(),"actor count mismatch");size_t i=0;
    for(const auto& item:expected){const auto& value=actual[i++];require(value.process==item.process && value.thread==item.thread && value.action==item.action,"actor/action mismatch");}
}
int main() {
 try {
    static_assert(std::is_trivially_copyable_v<ActorAction> && sizeof(ActorAction)<=3*sizeof(int));
    sys_state source;for(int i=0;i<NP;++i)source.status[i]=dstatus_exit(0);source.status[0]=0;
    auto& child=source.child[0];child.tv={1,0};
    for(int i=0;i<2;++i){thread_state thread;thread.tid=i+1;thread.alive=true;thread.is_main=i==0;child.threads.push_back(thread);}
    const auto original=serialized(child);
    expect(enabled_actor_actions(source),{{0,0,TransitionAction::Normal},{0,1,TransitionAction::Normal}});
    require(serialized(child)==original,"actor enumeration changed saved source");
    child.current_thread_idx=1;
    expect(enabled_actor_actions(source),{{0,0,TransitionAction::Normal},{0,1,TransitionAction::Normal}});
    child.threads[0].pending_syscall=timer(1000);child.threads[1].pending_syscall=timer(2000);
    expect(enabled_actor_actions(source),{{0,0,TransitionAction::Timeout},{0,1,TransitionAction::Timeout}});
    require(timeout_action_clock(source,0,0).tv_usec==1 && timeout_action_clock(source,0,1).tv_usec==2,"timeouts forced to global earliest deadline");
    PendingSyscall infinite;infinite.nr=SYS_pselect6;infinite.reason=PendingReason::Timer;
    child.threads[0].pending_syscall=infinite;
    expect(enabled_actor_actions(source),{{0,1,TransitionAction::Timeout}});
    child.threads[1].pending_syscall->phase=PendingPhase::Ready;
    expect(enabled_actor_actions(source),{{0,1,TransitionAction::Normal}});
    bool rejected=false;try{timeout_action_clock(source,0,1);}catch(const ActionRejected&){rejected=true;}
    require(rejected,"Ready syscall admitted as timeout");
    source.property_failed=true;expect(enabled_actor_actions(source),{});source.property_failed=false;
    child.threads[1].pending_syscall->phase=static_cast<PendingPhase>(99);
    rejected=false;try{enabled_actor_actions(source);}catch(const ActionRejected&){throw std::runtime_error("corrupt graph disguised as ineligible action");}catch(const std::runtime_error&){rejected=true;}
    require(rejected,"invalid pending graph was silently dropped");
    child.threads[0].pending_syscall.reset();child.threads[1].pending_syscall.reset();
    auto manager=std::make_shared<FdManager>(child.fd_manager_state);
    child.fs_state.set_fd_manager(manager);child.sock_state.set_fd_manager(manager);
    const int epoll=child.sock_state.do_epoll_create1(0);int fds[2];require(child.fs_state.do_pipe(fds,0)==0,"pure pipe fixture");
    EpollInstance watched;watched.epoll_fd=epoll;watched.watched_fds[fds[0]]={EPOLLIN,0};
    std::map<int,Socket> sockets;std::map<int,std::deque<UdpDatagram>> datagrams;
    std::map<int,TcpConnection> connections;std::map<int,EpollInstance> epolls{{epoll,watched}};
    std::ostringstream output;{cereal::BinaryOutputArchive archive(output);archive(sockets,datagrams,connections,epolls);}
    std::istringstream input(output.str());{cereal::BinaryInputArchive archive(input);archive(child.sock_state);}
    child.fd_manager_state=*manager;
    PendingSyscall io;io.nr=SYS_epoll_wait;io.reason=PendingReason::Io;io.args[0]=epoll;io.args[2]=1;io.args[3]=10;
    io.deadline=PendingDeadline{CLOCK_MONOTONIC,1,10000,0};child.threads[1].pending_syscall=io;
    expect(enabled_actor_actions(source),{{0,0,TransitionAction::Normal},{0,1,TransitionAction::Timeout}});
    child.fs_state.pipes_.at(child.fs_state.pipe_fds_.at(fds[0])).buffer={'x'};
    rejected=false;try{timeout_action_clock(source,0,1);}catch(const ActionRejected&){rejected=true;}
    require(rejected,"ready saved I/O admitted as timeout");
    expect(enabled_actor_actions(source),{{0,0,TransitionAction::Normal}});
    child.threads[1].pending_syscall->phase=PendingPhase::Retry;
    expect(enabled_actor_actions(source),{{0,0,TransitionAction::Normal},{0,1,TransitionAction::Normal}});
    child.threads[1].pending_syscall.reset();child.threads[1].alive=false;
    expect(enabled_actor_actions(source),{{0,0,TransitionAction::Normal}});
    source.status[0]=dstatus_exit(0);expect(enabled_actor_actions(source),{});
    if constexpr(NP>1) {
        source.status[1]=0;thread_state other;other.tid=1;other.alive=true;other.is_main=true;source.child[1].threads.push_back(other);
        expect(enabled_actor_actions(source),{{1,0,TransitionAction::Normal}});
    }
    puts("PASS pure saved actor actions: all runnable/Ready/Retry, finite nonmain/all deadlines, saved I/O readiness, dead/property terminal and corrupt graph");return 0;
 }catch(const std::exception& error){std::fprintf(stderr,"native actor policy: %s\n",error.what());return 1;}
}
