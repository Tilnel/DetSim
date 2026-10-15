#include "state/state.h"
#include "runtime_profile.h"
#include "ncurses_ui.h"
extern "C" detsim::ui::NCursesUI* get_ncurses_ui() { return nullptr; }
#include "engine/signal_handler.h"
#include "guest.h"
#include "scheduler.h"
#include "monitor.h"
#include "engine/thread_pool.h"
#include "engine/thread_control.h"
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/rseq.h>
#include <array>
#include <elf.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <cstdio>
#include <sys/syscall.h>
#include <unistd.h>

static void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
static bool rejected(const nlohmann::json& value) {
  try { detsim::configure_runtime_profile(value); }
  catch (const std::exception&) { return true; }
  return false;
}
static void check_initial_auxv(const char* executable) {
  pid_t pid = fork();
  require(pid >= 0, "fork auxv fixture");
  if (pid == 0) {
    if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) < 0) _exit(101);
    execl(executable, executable, "--auxv-fixture", nullptr);
    _exit(102);
  }
  int status = 0;
  auto cleanup = [&] { kill(pid, SIGKILL); waitpid(pid, &status, 0); };
  try {
    require(waitpid(pid, &status, 0) == pid && WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP,
            "fixture stopped immediately after exec");
    require(patch_at_random(pid) == 0, "ELF AT_RANDOM normalization succeeds");
    remove_vdso(pid);
    user_regs_struct regs{};
    require(ptrace(PTRACE_GETREGS, pid, nullptr, &regs) == 0, "read initial stack registers");
    uintptr_t argc = 0, word = 0;
    require(guest_read_exact(pid, reinterpret_cast<void*>(regs.rsp), &argc, sizeof(argc)), "read argc");
    uintptr_t pos = regs.rsp + (argc + 2) * sizeof(uintptr_t);
    do {
      require(guest_read_exact(pid, reinterpret_cast<void*>(pos), &word, sizeof(word)), "read environment pointer");
      pos += sizeof(word);
    } while (word);
    bool normalized_random = false;
    for (size_t i = 0; i < 256; ++i, pos += sizeof(Elf64_auxv_t)) {
      Elf64_auxv_t entry{};
      require(guest_read_exact(pid, reinterpret_cast<void*>(pos), &entry, sizeof(entry)), "read auxv");
      if (entry.a_type == AT_NULL) break;
      require(entry.a_type != AT_SYSINFO_EHDR, "initial vDSO entry disabled");
      if (entry.a_type == AT_RANDOM) {
        uint64_t bytes[2]{};
        require(guest_read_exact(pid, reinterpret_cast<void*>(entry.a_un.a_val), bytes, sizeof(bytes)), "read random bytes");
        normalized_random = bytes[0] == 0xdeadbeefcafebabeULL && bytes[1] == 0x1122334455667788ULL;
      }
    }
    require(normalized_random, "full AT_RANDOM matches deterministic ELF platform value");
  } catch (...) { cleanup(); throw; }
  cleanup();
  require(patch_at_random(-1) < 0, "missing tracee AT_RANDOM fails explicitly");
  bool failed = false;
  try { remove_vdso(-1); } catch (const std::runtime_error&) { failed = true; }
  require(failed, "missing tracee vDSO normalization fails explicitly");
}
static void check_startup_exit_diagnostic() {
  pid_t pid = fork();
  require(pid >= 0, "fork early-exit startup fixture");
  if (pid == 0) {
    if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) < 0) _exit(101);
    raise(SIGSTOP);
    syscall(SYS_exit_group, 17);
    _exit(102);
  }
  auto cleanup = [&] {
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) == 0) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
    }
  };
  try {
    int status = 0;
    require(waitpid(pid, &status, 0) == pid && WIFSTOPPED(status), "early-exit fixture stopped");
    require(ptrace(PTRACE_SETOPTIONS, pid, nullptr, PTRACE_O_TRACESYSGOOD) == 0, "enable syscall stops");
    bool diagnosed = false;
    try { extract_one_syscall(pid); }
    catch (const std::runtime_error& error) {
      std::string message = error.what();
      diagnosed = message.find("RuntimeProfile") != std::string::npos &&
                  message.find("exit status 17") != std::string::npos;
    }
    require(diagnosed, "startup exit reports actionable boundary error instead of assert");
  } catch (...) { cleanup(); throw; }
  cleanup();
}
alignas(32) static std::array<unsigned char,32> startup_rseq_area{};
static void check_startup_group_isolation() {
  const auto parent_group=getpgrp();
  for (int number : {SYS_setpgid,SYS_setsid}) {
    const pid_t pid=fork();require(pid>=0,"fork group prelude fixture");
    if (!pid) {
      if (setpgid(0,0) || ptrace(PTRACE_TRACEME,0,nullptr,nullptr)) _exit(109);
      asm volatile("int3");
      syscall(number,0,parent_group);_exit(110);
    }
    auto cleanup=[&] {kill(pid,SIGKILL);waitpid(pid,nullptr,0);};
    try {
      int status=0;
      require(waitpid(pid,&status,0)==pid && WIFSTOPPED(status),"group prelude stop");
      require(ptrace(PTRACE_SETOPTIONS,pid,nullptr,PTRACE_O_TRACESYSGOOD|PTRACE_O_EXITKILL)==0,
              "group prelude options");
      bool refused=false;
      try {extract_one_syscall(pid);}
      catch(const std::runtime_error& error) {
        refused=std::string(error.what()).find("owned process group")!=std::string::npos;
      }
      require(refused && getpgid(pid)==pid,"prelude changed process group before rejection");
    } catch (...) {cleanup();throw;}
    cleanup();
  }
}
static uint64_t startup_robust_head[3]{},startup_robust_out=0,startup_robust_length=0;
static uint32_t startup_tid_word=0xaabbccdd;
static volatile long startup_tid_result=0;
static uint64_t startup_new_mask=uint64_t{1}<<(SIGUSR2-1),startup_old_mask=0;
static void check_startup_tid_contract() {
  const auto old_child=ptmc_state.running_state.child[0];
  const int old_pid=ptmc_state.pids[0],old_cursor=ptmc_state.cursor;
  pid_t pid=fork();require(pid>=0,"fork TID prelude fixture");
  if (!pid) {
    if (__rseq_size && syscall(SYS_rseq,
            static_cast<char*>(__builtin_thread_pointer()) + __rseq_offset,
            32, RSEQ_FLAG_UNREGISTER, RSEQ_SIG)) _exit(107);
    if (mmap(reinterpret_cast<void*>(scratch_page),4096,PROT_READ|PROT_WRITE|PROT_EXEC,
             MAP_FIXED_NOREPLACE|MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==MAP_FAILED) _exit(104);
    auto* code=reinterpret_cast<unsigned char*>(scratch_page+0xff0);code[0]=0x0f;code[1]=0x05;
    const uint64_t inherited=uint64_t{1}<<(SIGUSR1-1);
    if (syscall(SYS_rt_sigprocmask,SIG_SETMASK,&inherited,nullptr,8)) _exit(106);
    if (syscall(SYS_set_robust_list,nullptr,24)) _exit(108);
    if (ptrace(PTRACE_TRACEME,0,nullptr,nullptr)) _exit(105);
    asm volatile("int3");
    syscall(SYS_rseq,startup_rseq_area.data(),32,0,RSEQ_SIG);
    syscall(SYS_rseq,reinterpret_cast<void*>(1),UINT32_MAX,UINT32_MAX,0);
    syscall(SYS_set_robust_list,startup_robust_head,24);
    syscall(SYS_get_robust_list,0,&startup_robust_out,&startup_robust_length);
    startup_tid_result=syscall(SYS_set_tid_address,&startup_tid_word);
    syscall(SYS_gettid);syscall(SYS_getpid);
    syscall(SYS_rt_sigprocmask,SIG_BLOCK,&startup_new_mask,&startup_old_mask,8);_exit(0);
  }
  auto cleanup=[&] {kill(pid,SIGKILL);waitpid(pid,nullptr,0);
    ptmc_state.running_state.child[0]=old_child;ptmc_state.pids[0]=old_pid;ptmc_state.cursor=old_cursor;};
  try {
    int status=0;require(waitpid(pid,&status,0)==pid && WIFSTOPPED(status),"TID prelude stop");
    require(ptrace(PTRACE_SETOPTIONS,pid,nullptr,PTRACE_O_TRACESYSGOOD|PTRACE_O_EXITKILL)==0,"TID prelude options");
    ptmc_state.pids[0]=pid;ptmc_state.cursor=0;ptmc_state.running_state.child[0]=tracee_state{};
    adopt_thread_signal_mask(0,pid);
    for (int attempt=0;attempt<2;++attempt) {
      const auto call=extract_one_syscall(pid);
      require(call.nr==SYS_rseq && static_cast<int64_t>(call.rval)==-ENOSYS,
              "prelude rseq did not return ENOSYS for all arguments");
    }
    std::array<unsigned char,32> observed_rseq{};
    require(guest_read_exact(pid,startup_rseq_area.data(),observed_rseq.data(),observed_rseq.size()) &&
            observed_rseq==startup_rseq_area,"prelude rseq changed user memory");
    __ptrace_rseq_configuration rseq_configuration{};
    require(ptrace(PTRACE_GET_RSEQ_CONFIGURATION,pid,reinterpret_cast<void*>(sizeof(rseq_configuration)),
                   &rseq_configuration)==sizeof(rseq_configuration) && !rseq_configuration.rseq_abi_pointer,
            "prelude leaked a native rseq registration");
    const auto robust_set=extract_one_syscall(pid),robust_get=extract_one_syscall(pid);
    uint64_t robust_out=0,robust_size=0;
    require(robust_set.nr==SYS_set_robust_list && robust_set.rval==0 &&
            robust_get.nr==SYS_get_robust_list && robust_get.rval==0 &&
            guest_read_exact(pid,&startup_robust_out,&robust_out,sizeof(robust_out)) &&
            guest_read_exact(pid,&startup_robust_length,&robust_size,sizeof(robust_size)) &&
            robust_out==reinterpret_cast<uint64_t>(startup_robust_head) && robust_size==24 &&
            ptmc_state.running_state.child[0].threads[0].robust_list_head==robust_out,
            "prelude robust-list registration or query lost logical state");
    void* actual_robust=nullptr;size_t actual_length=0;
    require(syscall(SYS_get_robust_list,pid,&actual_robust,&actual_length)==0 && !actual_robust,
            "prelude installed actual host robust list");
    auto registration=extract_one_syscall(pid);
    require(registration.nr==SYS_set_tid_address && registration.rval==uint64_t(logical_process_id(0)),
            "prelude set_tid_address returned a fabricated or physical TID");
    require(ptmc_state.running_state.child[0].threads[0].clear_child_tid==reinterpret_cast<uint64_t>(&startup_tid_word),
            "prelude lost clear-child-tid registration");
    require(extract_one_syscall(pid).rval==registration.rval && extract_one_syscall(pid).rval==registration.rval,
            "prelude gettid/getpid disagree with registration identity");
    const auto mask_call=extract_one_syscall(pid);
    uint64_t old_mask=0;
    const uint64_t inherited=uint64_t{1}<<(SIGUSR1-1);
    require(mask_call.nr==SYS_rt_sigprocmask && mask_call.rval==0 &&
            guest_read_exact(pid,&startup_old_mask,&old_mask,sizeof(old_mask)) && old_mask==inherited &&
            ptmc_state.running_state.child[0].threads[0].signal_mask==(inherited|startup_new_mask) &&
            read_thread_signal_mask(pid)==(inherited|startup_new_mask),
            "prelude signal mask did not use the logical task ABI");
    long observed=0;
    require(guest_read_exact(pid,const_cast<const long*>(&startup_tid_result),&observed,sizeof(observed)) &&
            observed==logical_process_id(0),"prelude guest did not receive logical ID");
    const auto address=scratch_page+0x300;
    require(tracee_do_syscall(pid,SYS_prctl,PR_GET_TID_ADDRESS,address,0,0,0,0)==0,"query physical prelude registration");
    uint64_t physical=1;uint32_t untouched=0;
    require(guest_read_exact(pid,reinterpret_cast<void*>(address),&physical,sizeof(physical)) && physical==0 &&
            guest_read_exact(pid,&startup_tid_word,&untouched,sizeof(untouched)) && untouched==0xaabbccdd,
            "prelude leaked a physical registration or wrote set_tid_address target");
  } catch (...) {cleanup();throw;}
  cleanup();
}

int main(int argc, char** argv) {
  if (argc > 1) return 103; // Parent never resumes the fixture beyond exec stop.

  using nlohmann::json;
  detsim::configure_runtime_profile(json::object());
  require(detsim::runtime_profile().startup_syscall == SYS_munmap, "declared dynamic-loader boundary");
  std::string default_identity = detsim::runtime_profile_identity();
  require(json::parse(default_identity).at("rseq")=="unavailable-enosys",
          "runtime identity omits rseq availability contract");
  require(detsim::runtime_profile().initial_time_us == 1000000, "default machine clock origin is nonzero");
  detsim::configure_runtime_profile({{"initial_time_us", 1234567}});
  require(detsim::runtime_profile().initial_time_us == 1234567 &&
          detsim::runtime_profile_identity() != default_identity, "clock origin enters execution identity");
  detsim::configure_runtime_profile({{"initial_time_us", 0}});
  require(detsim::runtime_profile().initial_time_us == 0, "explicit zero origin remains reproducible");
  require(rejected({{"initial_time_us", -1}}), "negative origin rejected");
  require(rejected({{"initial_time_us", 1.5}}), "fractional origin rejected");
  require(rejected({{"initial_time_us", uint64_t(-1)}}), "overflowing origin rejected");
  detsim::configure_runtime_profile(json::object());
  std::vector<int> boundaries(NP, SYS_mprotect);
  detsim::configure_runtime_profile({{"startup_syscalls", boundaries}});
  require(detsim::startup_syscall_for_node(0) == SYS_mprotect, "per-node startup boundary");
  require(detsim::runtime_profile_identity() != default_identity, "per-node boundaries enter identity");
  if (NP > 1) {
    boundaries[1] = SYS_munmap;
    detsim::configure_runtime_profile({{"startup_syscalls", boundaries}});
    require(detsim::startup_syscall_for_node(1) == SYS_munmap, "heterogeneous startup boundary");
  }
  require(rejected({{"startup_syscalls", std::vector<int>(NP + 1, SYS_munmap)}}), "wrong boundary vector size rejected");
  boundaries[0] = -1;
  require(rejected({{"startup_syscalls", boundaries}}), "invalid per-node syscall rejected");
  detsim::configure_runtime_profile(json::object());
  SignalHandler handler;
  SignalContext context;
  context.pid = getpid();
  context.signal = SIGURG;
  context.category = handler.classify(context.signal);
  bool threw = false;
  try { handler.handle(context); } catch (const std::runtime_error&) { threw = true; }
  require(threw, "SIGURG must not be guessed to be Go preemption");
  detsim::configure_runtime_profile({{"startup_syscall", SYS_mprotect}, {"ignored_signals", {SIGURG}}});
  require(detsim::runtime_profile().startup_syscall == SYS_mprotect, "explicit startup boundary");
  require(detsim::runtime_profile_identity() != default_identity, "runtime semantics included in identity");
  auto result = handler.handle(context);
  require(result.should_retry && sig::should_continue(result), "explicit ignored signal retries");
  require(rejected({{"ignored_signals", {SIGKILL}}}), "cannot ignore SIGKILL");
  require(rejected({{"startup_syscall", 11.5}}), "fractional syscall rejected");
  require(rejected({{"ignored_signals", {1.5}}}), "fractional signal rejected");
  require(rejected({{"startup_syscall", uint64_t(1) << 32}}), "overflowing syscall rejected");
  require(rejected({{"ignored_signals", {uint64_t(1) << 32}}}), "overflowing signal rejected");
  require(rejected({{"startup_syscall", -1}}), "invalid boundary rejected");
  require(rejected({{"startup_syscall_limit", 0}}), "unbounded startup rejected");
  require(rejected({{"unknown_policy", true}}), "misspelled profile key rejected");
  require(rejected({{"legacy_glibc_cpu_mask", true}}), "private glibc patch removed");
  require(rejected({{"ignored_signals", {SIGSEGV}}}), "fatal signal suppression rejected");
  detsim::configure_runtime_profile(json::object());
  require(detsim::runtime_profile_identity() == default_identity, "runtime identity canonical reset");
  require(!detsim::runtime_profile().ignored_signals.count(SIGURG), "profile reset has no leftover policy");
  check_initial_auxv(argv[0]);
  check_startup_exit_diagnostic();
  check_startup_tid_contract();
  check_startup_group_isolation();
  std::puts("PASS explicit runtime startup, ELF normalization and signal policy");
}
