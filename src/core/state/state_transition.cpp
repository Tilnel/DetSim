#include "state_transition.h"
#include <algorithm>
#include "state_view.h"
#include "plugins.h"
#include "common.h"
#include "runtime_profile.h"
#include "engine/thread_pool.h"
#include "engine/pending_syscall.h"
#include "pending_io.h"
#include <stdexcept>
#include <algorithm>
#include "emu.h"
#include "fsstate.h"
#include "futexstate.h"
#include "guest.h"
#include "log_wrapper.h"
#include "ncurses_ui.h"
#include "proc_status.h"
#include "state_store.h"
#include "sysstate_store.h"
#include "utils.h"
#include "../engine/exec_engine.h"
#include "../engine/thread_manager.h"
#include "../engine/signal_handler.h"
#include "../syscall/dispatcher.h"
#include <fcntl.h>
#include <readline/readline.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#include <monitor.h>

#define pids ptmc_state.pids

// Track VFS mmap operations between enter and exit
struct VfsMmapState
{
  bool active;
  int fd;
  off_t offset;
  size_t length;
};
static VfsMmapState g_vfs_mmap = {false, -1, 0, 0};

int analyze_choose(struct syscall_info &info)
{
  // Check if syscall has predefined choose count
  int predefined = choose_many[info.nr];
  if (predefined > 0)
  {
    return predefined;
  }

  // For recvfrom, check number of available messages for message reordering
  if (info.nr == SYS_recvfrom)
  {
    int fd = info.args[0];
    int choices = emu_recvfrom_get_choices(fd);
    // Only provide choice when there are 2+ messages (allow reordering)
    if (choices >= 2)
    {
      return 2; // 0=receive first message, 1=receive second message
    }
    return 0; // No choice when < 2 messages
  }

  return 0;
}

extern "C" detsim::ui::NCursesUI *get_ncurses_ui();
void handle_user_choice()
{
  // Check if there's a preset choice from batch file
  if (ptmc_state.batch_choice_preset >= 0 &&
      ptmc_state.batch_choice_preset < ptmc_state.n_choose)
  {
    ptmc_state.choose = ptmc_state.batch_choice_preset;
    ptmc_state.batch_choice_preset = -1; // Clear preset after use
    UI_LOG_INFO("Using batch preset choice: %d", ptmc_state.choose);
    return;
  }

  detsim::ui::NCursesUI *ui = get_ncurses_ui();
  if (ui)
  {
    /* Use NCursesUI prompt */
    char prompt_buf[256];
    snprintf(prompt_buf, sizeof(prompt_buf), "Choose from %d options",
             ptmc_state.n_choose);
    ptmc_state.choose =
        ui->prompt_int(prompt_buf, 0, 0, ptmc_state.n_choose - 1);
  }
  else
  {
    /* Fallback to readline */
    static char *line_read;
    UI_LOG_INFO("Here comes with %d choices.", ptmc_state.n_choose);
    line_read = readline("You choose? [0, N): ");
    while (sscanf(line_read, "%d", &ptmc_state.choose) != 1 ||
           ptmc_state.choose < 0 ||
           ptmc_state.choose >= ptmc_state.n_choose)
    {
      free(line_read);
      line_read = readline("Please make legal choice: ");
    }
    free(line_read);
  }
}

void handle_auto_choice()
{
  if (ptmc_state.n_choose == 0)
    return;

  switch (ptmc_state.mode)
  {
    case PTMC_STATE::MODE_RAND:
      ptmc_state.choose = rand() % ptmc_state.n_choose;
      break;
    case PTMC_STATE::MODE_DFS:
      if (ptmc_state.choose < 0)
      {
        ptmc_state.choose = rand() % ptmc_state.n_choose;
      }
      break;
    default:
      break;
  }
}

int exec_once(const sys_state &s, syscall_info &info)
{
  ExecutionEngine* engine = exec::get_engine();
  if (!engine) {
    LOG_ERROR("ExecutionEngine not initialized");
    return CKPT_STOP;
  }

  ExecResult result = engine->execute_step(s, info);

  switch (result.status) {
    case ExecStatus::EXIT:
      LOG_INFO("Tracee %d exited", ptmc_state.cursor);
      break;
    case ExecStatus::CRASH:
      LOG_INFO("Tracee %d crashed", ptmc_state.cursor);
      break;
    case ExecStatus::DISCARD:
      LOG_DEBUG("Tracee %d execution discarded", ptmc_state.cursor);
      break;
    default:
      break;
  }

  return result.checkpoint_decision;
}

static bool check_assertions(const sys_state& state, unsigned actor)
{
  if (ptmc_state.assertions.empty()) return false;
  const detsim::StateView view(state);
  bool violated = false;
  for (const auto& assertion : ptmc_state.assertions) {
    if (!view.evaluate(actor, assertion)) {
      detsim::ui::ui_printf("Assertion \"%s\" failed.\n", assertion.c_str());
      violated = true;
    }
  }
  return violated;
}

TransitionResult state_transition(const sys_state &source_state,
                                  int process_index, int requested_thread, TransitionAction action)
{
  if (&source_state == &ptmc_state.running_state) {
    const sys_state detached_source=source_state;
    return state_transition(detached_source,process_index,requested_thread,action);
  }
  if (process_index < 0 || process_index >= NP)
    throw std::runtime_error("Transition process index out of range");
  if (action != TransitionAction::Normal && action != TransitionAction::Timeout)
    throw std::runtime_error("Invalid transition action");
  const auto preflight_timeout = [&](const tracee_state& child) {
    if (DISDEAD(source_state.status[process_index]) || source_state.property_failed)
      throw ActionRejected("actor belongs to a terminal state");
    validate_pending_syscalls(child);
    if (requested_thread < 0 || requested_thread >= static_cast<int>(child.threads.size()))
      throw ActionRejected("thread index out of range");
    const auto& actor = child.threads[requested_thread];
    if (!actor.alive || !actor.pending_syscall || actor.pending_syscall->phase != PendingPhase::Waiting || !actor.pending_syscall->deadline)
      throw ActionRejected("actor requires a waiting syscall with a finite deadline");
    const auto& pending = *actor.pending_syscall;
    syscall_info call{}; call.nr = pending.nr;
    std::copy(std::begin(pending.args), std::end(pending.args), call.args);
    if (pending.reason == PendingReason::Io && io_retry_ready(child, call))
      throw ActionRejected("I/O is already ready");
    return pending_timeout_clock(child.tv, *pending.deadline);
  };
  if (action == TransitionAction::Timeout) {
    if (requested_thread == -1) requested_thread = source_state.child[process_index].current_thread_idx;
    (void)preflight_timeout(source_state.child[process_index]);
  }
  if (DISDEAD(source_state.status[process_index]))
    throw std::runtime_error("Cannot execute a terminated process; load a predecessor state first");
  if (source_state.property_failed)
    throw std::runtime_error("Cannot execute a terminal property violation; load a predecessor state first");
  if (requested_thread < -1 || requested_thread >= static_cast<int>(source_state.child[process_index].threads.size()))
    throw std::runtime_error("Transition thread index out of range");
  if (action == TransitionAction::Normal && requested_thread >= 0) {
    const auto& actor=source_state.child[process_index].threads[requested_thread];
    if (!actor.alive || (actor.pending_syscall && actor.pending_syscall->phase == PendingPhase::Waiting))
      throw std::runtime_error("Transition thread is not runnable");
  }
  TransitionResult result;
  ptmc_state.cursor = process_index;

  // restore state to tracees with timing
  auto t1 = std::chrono::high_resolution_clock::now();
  source_state.recover_running_state();
  auto t2 = std::chrono::high_resolution_clock::now();
  result.restore_time =
      std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1);

  if (requested_thread >= 0) {
    ptmc_state.current_thread_idx[process_index]=requested_thread;
    ptmc_state.running_state.child[process_index].current_thread_idx=requested_thread;
  }

  // do syscalls - copy per-thread syscall info for each process
  syscall_info new_syscalls[NP][MAX_THREADS_PER_PROCESS];
  for (int i = 0; i < NP; i++)
    memcpy(new_syscalls[i], source_state.child[i].si, sizeof(syscall_info) * MAX_THREADS_PER_PROCESS);

  // Get current thread index for the active process
  int thread_idx = ptmc_state.current_thread_idx[process_index];
  syscall_info executed = new_syscalls[process_index][thread_idx];
  int ckpt;
  if (action == TransitionAction::Timeout) {
    const auto target = preflight_timeout(ptmc_state.running_state.child[process_index]);
    const auto& actor = ptmc_state.running_state.child[process_index].threads.at(thread_idx);
    const auto logical_tid = actor.tid;
    const auto physical_tid = actor.physical_tid;
    ptmc_state.time[process_index] = target;
    ptmc_state.tracee_image_dirty[process_index] = true;
    if (!expire_pending_syscall(process_index, logical_tid) ||
        !complete_pending_syscall(process_index, physical_tid, executed))
      throw std::runtime_error("Admitted timeout failed to complete");
    ckpt = CKPT_YES;
  } else {
    ckpt = exec_once(source_state, executed);
  }
  const int transition_choice = action == TransitionAction::Timeout ? DETSIM_CHOICE_TIMEOUT :
      (ptmc_state.n_choose ? ptmc_state.choose : -1);
  // Automatic execution may skip a blocked/exited thread. Attribute the
  // completed syscall to the actor that actually ran, preserving other slots.
  thread_idx = ptmc_state.current_thread_idx[process_index];
  new_syscalls[process_index][thread_idx] = executed;
  // A modeled clone publishes a stopped logical child during this transition.
  // Carry its initial child-side syscall result into the candidate snapshot.
  const auto& runtime_child=ptmc_state.running_state.child[process_index];
  for (size_t slot=source_state.child[process_index].threads.size(); slot<runtime_child.threads.size(); ++slot)
    new_syscalls[process_index][slot]=runtime_child.si[slot];

  // Grant readiness without consuming a peer's data or publishing its return.
  refresh_pending_syscalls();

  // save state with timing
  auto t3 = std::chrono::high_resolution_clock::now();
  result.new_state = sys_state(new_syscalls, &source_state, false);
  if (ckpt != CKPT_DISCARD) {
    detsim_event_v2 event{};
    event.process = process_index; event.thread = thread_idx;
    event.syscall_number = executed.nr;
    event.choice = transition_choice;
    std::copy(std::begin(executed.args), std::end(executed.args), event.args);
    const bool unfinished=result.new_state.child[process_index].threads.at(thread_idx).pending_syscall.has_value();
    if (ckpt==CKPT_STOP) {
      event.phase=DETSIM_EXECUTION_STOPPED;
      event.syscall_number=-1;event.choice=-1;
      std::fill(std::begin(event.args),std::end(event.args),0);
    } else if (executed.nr==SYS_exit || executed.nr==SYS_exit_group) {
      event.phase=DETSIM_THREAD_EXITED;
    } else {
      event.phase=unfinished?DETSIM_SYSCALL_BLOCKED:DETSIM_SYSCALL_COMPLETED;
      event.result_valid=!unfinished;
      event.result=unfinished?0:static_cast<int64_t>(executed.rval);
    }
    result.property_violation = detsim::plugins::check_transition(source_state, result.new_state, event);
    result.property_violation |= check_assertions(result.new_state, process_index);
  }
  // Monitor state is committed atomically with the product-state identity.
  result.new_state.property_failed = result.property_violation;
  result.new_state.commit_metadata();
  auto t4 = std::chrono::high_resolution_clock::now();
  result.save_time =
      std::chrono::duration_cast<std::chrono::microseconds>(t4 - t3);
  result.code = static_cast<TransitionResultCode>(ckpt);

  // add to state tree for future exploration
  state_tree_add(source_state, result.new_state, process_index,
                 transition_choice);
  ptmc_state.running_state = result.new_state;
  for (int node=0; node<NP; ++node) ptmc_state.tracee_image_dirty[node]=false;
  return result;
}

static pid_t tracer_owner = 0;
static SysStateStore *metadata_store = nullptr;
static StateStore *snapshot_store = nullptr;

// Only the tracer owns these children. Never signal its whole process group:
// that group may also contain a shell, test driver, or unrelated workload.
static void terminate_tracees()
{
  if (getpid() != tracer_owner)
    return;
  for (int i = 0; i < NP; ++i)
  {
    pid_t child = pids[i];
    if (child <= 0)
      continue;
    int status = 0;
    pid_t result;
    do {
      result = waitpid(child, &status, WNOHANG);
    } while (result < 0 && errno == EINTR);
    // An exited/reaped tracee PID may have been reused: only kill an owned child.
    if (result >= 0 && !(result == child && (WIFEXITED(status) || WIFSIGNALED(status))))
    {
      const bool isolated = getpgid(child) == child;
      kill(child, SIGKILL);
      // A ptraced group leader cannot be reaped before its worker tasks.
      // Wait only for this tracee's dedicated group, never unrelated children.
      if (isolated) {
        for (;;) {
          const auto reaped = waitpid(-child, nullptr, __WALL);
          if (reaped > 0 || (reaped < 0 && errno == EINTR)) continue;
          break;
        }
      } else {
        // Startup failed before group isolation; no guest threads could start.
        while (waitpid(child, nullptr, __WALL) < 0 && errno == EINTR) {}
      }
    }
    pids[i] = 0;
  }
}

static void exit_all(int sig)
{
  // No locks, logging, allocation, or store shutdown from a signal handler.
  terminate_tracees();
  _exit(128 + sig);
}

static void exit_all(void)
{
  if (getpid() != tracer_owner) return;
  // Persistence failures must not turn into a throwing atexit handler or a
  // successful checker exit. Always reap owned tracees, even after I/O failure.
  bool failed = false;
  try { if (snapshot_store) snapshot_store->shutdown(); }
  catch (const std::exception &error) {
    fprintf(stderr, "Snapshot shutdown failed: %s\n", error.what());
    failed = true;
  }
  try { if (metadata_store) metadata_store->shutdown(); }
  catch (const std::exception &error) {
    fprintf(stderr, "Metadata shutdown failed: %s\n", error.what());
    failed = true;
  }
  terminate_tracees();
  if (failed) _Exit(EXIT_FAILURE);
}

static int resume_startup_syscall(pid_t pid)
{
  ptrace_right(PTRACE_SYSCALL, pid, nullptr, nullptr);
  for (;;) {
    int status = 0;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited != pid)
      throw std::runtime_error("Cannot wait for tracee during configured startup prelude");
    if (!WIFSTOPPED(status)) {
      const std::string result = WIFEXITED(status) ?
          "exit status " + std::to_string(WEXITSTATUS(status)) :
          "signal " + std::to_string(WTERMSIG(status));
      throw std::runtime_error("Startup prelude ended with " + result +
          " before reaching the configured boundary; check RuntimeProfile startup_syscall/startup_syscalls");
    }
    if (WSTOPSIG(status) == PTRACE_TRAP_SIG) return status;
    try {
      auto signal = sig::process(pid, status);
      if (!sig::should_continue(signal))
        throw std::runtime_error("tracee terminated by signal " + std::to_string(WSTOPSIG(status)));
    } catch (const std::exception& error) {
      throw std::runtime_error(std::string("Startup prelude did not reach configured RuntimeProfile boundary: ") + error.what());
    }
    ptrace_right(PTRACE_SYSCALL, pid, nullptr, nullptr);
  }
}

/* Extract one entire syscall, preserve its original effect */
syscall_info extract_one_syscall(pid_t pid)
{
  /* ptrace(2): When delivering system call traps, set bit 7 in the signal     *
   * number (i.e., deliver SIGTRAP|0x80). This makes it easy for the tracer to *
   * distinguish normal traps from those caused by a system call. */
  struct ptrace_syscall_info info;
  syscall_info ret = {}; /* Zero-initialize all fields */

  /* entry */
  resume_startup_syscall(pid);

  ptrace_right(PTRACE_GET_SYSCALL_INFO, pid, (void *)sizeof(info), &info);

  if (info.op != PTRACE_SYSCALL_INFO_ENTRY)
    throw std::runtime_error("Unexpected syscall phase before configured RuntimeProfile startup boundary");
  ret.nr = info.entry.nr;
  for (int i = 0; i < 6; i++)
    ret.args[i] = info.entry.args[i];

  // Thread ownership begins at exec, even in the trusted loader prelude.
  // The prelude must end before births; children cannot run outside the model.
  if (ret.nr == SYS_clone || ret.nr == SYS_clone3 || ret.nr == SYS_fork || ret.nr == SYS_vfork)
    throw std::runtime_error("RuntimeProfile startup boundary must precede thread creation");
  if (ret.nr == SYS_setpgid || ret.nr == SYS_setsid)
    throw std::runtime_error("Startup prelude cannot change the owned process group");
  if (ret.nr == SYS_set_tid_address) tracee_set_rdi(pid,0);
  // The execution environment exposes no rseq syscall, including the loader.
  // A successful initial registration followed by child ENOSYS breaks libc.
  if (ret.nr == SYS_rseq || ret.nr == SYS_set_robust_list || ret.nr == SYS_get_robust_list) {
    tracee_set_orig_rax(pid,-1);tracee_set_rax(pid,-1);
  }
  if (ret.nr == SYS_rt_sigprocmask) {
    if (ptmc_state.running_state.child[ptmc_state.cursor].threads.empty())
      adopt_thread_signal_mask(ptmc_state.cursor,pid);
    tracee_set_orig_rax(pid,-1);tracee_set_rax(pid,-1);
  }

  /* exit */
  resume_startup_syscall(pid);

  ptrace_right(PTRACE_GET_SYSCALL_INFO, pid, (void *)sizeof(info), &info);
  if (info.op != PTRACE_SYSCALL_INFO_EXIT)
    throw std::runtime_error("Unexpected syscall phase before configured RuntimeProfile startup boundary");
  ret.rval = info.exit.rval;

  if (ret.nr == SYS_set_robust_list) {
    ret.rval=emulate_set_robust_list(ptmc_state.cursor,pid,ret.args[0],ret.args[1]);
    tracee_set_rax(pid,ret.rval);
  } else if (ret.nr == SYS_get_robust_list) {
    ret.rval=emulate_get_robust_list(ptmc_state.cursor,pid,static_cast<int32_t>(ret.args[0]),ret.args[1],ret.args[2]);
    tracee_set_rax(pid,ret.rval);
  } else if (ret.nr == SYS_rseq) {
    ret.rval = -ENOSYS;
    tracee_set_rax(pid,ret.rval);
  } else if (ret.nr == SYS_rt_sigprocmask) {
    ret.rval=emulate_thread_sigprocmask(ptmc_state.cursor,pid,ret.args[0],ret.args[1],ret.args[2],ret.args[3]);
    tracee_set_rax(pid,ret.rval);
  } else if (ret.nr == SYS_set_tid_address) {
    if (static_cast<int64_t>(ret.rval)<0)
      throw std::runtime_error("Cannot detach startup clear_child_tid registration");
    tracee_set_rdi(pid,ret.args[0]);
    ret.rval=record_logical_clear_tid(ptmc_state.cursor,pid,ret.args[0]);
    tracee_set_rax(pid,ret.rval);
  } else if (ret.nr == SYS_getpid || ret.nr == SYS_gettid) {
    ret.rval=logical_process_id(ptmc_state.cursor);
    tracee_set_rax(pid,ret.rval);
  }
  /* getrandom */
  if (ret.nr == SYS_getrandom && ret.rval > 0)
  {
    const unsigned char zeros[4096]{};
    size_t remaining = size_t(ret.rval);
    if (remaining > ret.args[1])
      throw std::runtime_error("Invalid startup getrandom result length");
    uintptr_t address = ret.args[0];
    while (remaining) {
      size_t length = std::min(remaining, sizeof(zeros));
      if (!guest_write_exact(pid, reinterpret_cast<void*>(address), zeros, length))
        throw std::runtime_error("Cannot normalize startup getrandom output");
      address += length;
      remaining -= length;
    }
  }
  if (ret.nr == SYS_getcpu && ret.rval == 0)
  {
    const unsigned int zero = 0;
    for (int argument = 0; argument < 2; ++argument)
      if (ret.args[argument] &&
          !guest_write_exact(pid, reinterpret_cast<void*>(ret.args[argument]), &zero, sizeof(zero)))
        throw std::runtime_error("Cannot normalize startup getcpu output");
  }

  return ret;
}

static void init_tracee_state(int index)
{
  int pid = pids[index];
  int wstatus = 0;
  const auto& profile = detsim::runtime_profile();
  // A deterministic machine clock origin, configurable independently of the
  // target. Nonzero by default; this is not automatic elapsed-time advancement.
  ptmc_state.time[index] = {profile.initial_time_us / 1000000,
                           profile.initial_time_us % 1000000};
  const int stop_nr = detsim::startup_syscall_for_node(index);
  LOG_INFO("Startup prelude boundary: syscall %d (limit %zu)",
           stop_nr, profile.startup_syscall_limit);
  if (waitpid(pid, &wstatus, 0) != pid || !WIFSTOPPED(wstatus) || WSTOPSIG(wstatus) != SIGSTOP)
    throw std::runtime_error("Tracee failed to reach initial startup stop");
  /* tracee: stop at raise(SIGSTOP) */

  /* ptrace_right(PTRACE_SEIZE, pid, 0, 0); */
  ptrace_right(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL);

  /* skip execve */
  while (extract_one_syscall(pid).nr != SYS_execve)
    ;

  /* execve(2): after a successful execve, a SIGTRAP *
   * (instead of SIGTRAP | 0x80) will be sent to it  */
  ptrace_right(PTRACE_SYSCALL, pid, 0, 0);
  if (waitpid(pid, &wstatus, 0) != pid || !WIFSTOPPED(wstatus) || WSTOPSIG(wstatus) != SIGTRAP)
    throw std::runtime_error("Tracee failed to exec before configured RuntimeProfile startup boundary");

  // exec replaced the old user image; adopt its inherited task mask before
  // executing the loader prelude through the same rt_sigprocmask model.
  ptmc_state.running_state.child[index]=tracee_state{};
  adopt_thread_signal_mask(index,pid);
  if (profile.disable_vdso) remove_vdso(pid);
  if (profile.deterministic_at_random && patch_at_random(pid) != 0)
    throw std::runtime_error("Cannot normalize tracee AT_RANDOM");

  bool reached = false;
  for (size_t count = 0; count < profile.startup_syscall_limit; ++count) {
    if (extract_one_syscall(pid).nr == stop_nr) { reached = true; break; }
  }
  if (!reached)
    throw std::runtime_error("Runtime startup syscall boundary was not reached");

  /* Preserve memory for parameters. Need recover registers *
   * for the original first syscall. */
  tracee_reserve_temp_page(pid);
  tracee_write_mem(pid, (void *)(scratch_page + 0xff0),
                   "\x0f\x05\x0f\x05\x0f\x05\x0f\x05", 8);
  capture_process_kernel_state(pid,index);
  adopt_clear_child_tid(index,pid);
  adopt_thread_robust_list(index,pid);
}

/* prepare tracee for ptrace, and execve */
static void start_tracee(int id)
{
  signal(SIGINT, SIG_IGN);
  ptrace_right(PTRACE_TRACEME, 0, NULL, NULL);
  // detsim::ui::ui_printf("Will start executable %s\n",
  // ptmc_state.tracee[id].executable);
  if (log_fp != stdout)
    fclose(log_fp);
  raise(SIGSTOP);

  execv(ptmc_state.tracee[id].executable, ptmc_state.tracee[id].argv.data());

  perror("exec");
  _exit(127);
}

int state_initialization()
{
  clear_snapshot_thread_bindings();
  for (int node=0; node<NP; ++node) ptmc_state.tracee_image_dirty[node]=false;
  /* initialization for multiple process */
  // setpgid(0, pgid);
  setbuf(stdout, NULL);
  tracer_owner = getpid();
  // Construct before registering cleanup, so the singleton outlives this callback.
  snapshot_store = &StateStore::instance();
  metadata_store = &SysStateStore::instance();
  atexit(exit_all);
  signal(SIGTERM, exit_all);
  // signal(SIGINT, exit_all);

  // The cleanup wait set must include retained worker carriers, without
  // touching the shell, driver or another tracee. Publish groups atomically
  // with respect to our SIGTERM handler.
  sigset_t blocked, previous_mask;
  sigemptyset(&blocked);sigaddset(&blocked,SIGTERM);
  if (sigprocmask(SIG_BLOCK,&blocked,&previous_mask))
    throw std::runtime_error("Cannot protect tracee startup from SIGTERM");
  try {
    for (int i=0;i<NP;++i) {
      const pid_t child=fork();
      if (child<0) throw std::runtime_error("Cannot fork tracee");
      if (!child) {
        if (setpgid(0,0) || prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=tracer_owner)
          _exit(126);
        if (sigprocmask(SIG_SETMASK,&previous_mask,nullptr)) _exit(126);
        start_tracee(i);
        _exit(127);
      }
      pids[i]=child;
      if (setpgid(child,child) && getpgid(child)!=child)
        throw std::runtime_error("Cannot isolate tracee process group");
    }
  } catch (...) {
    sigprocmask(SIG_SETMASK,&previous_mask,nullptr);
    throw;
  }
  if (sigprocmask(SIG_SETMASK,&previous_mask,nullptr))
    throw std::runtime_error("Cannot restore tracer signal mask after startup");

  detsim::ui::ui_printf("Instance #pid = %d\n", getpid());

  /* Initialize FdManagers and link to states */
  for (int i = 0; i < NP; i++)
  {
    ptmc_state.fd_managers[i] = std::make_shared<FdManager>();
    ptmc_state.fs_states[i].set_fd_manager(ptmc_state.fd_managers[i]);
    ptmc_state.sock_states[i].set_fd_manager(ptmc_state.fd_managers[i]);

  /* Initialize execution engine */
  exec::init_all();
  }

  /* here goes to first state */
  /* initialize tracees' state */
  for (int i = 0; i < NP; i++)
  {
    ptmc_state.cursor = i;
    init_tracee_state(i);
  }

  /* Record first sys_state. Notice that the syscall_info need no recording */
  struct syscall_info syscall_info[NP][MAX_THREADS_PER_PROCESS];
  memset(syscall_info, 0, sizeof(syscall_info));
  ptmc_state.cursor = -1;

  sys_state first_state(syscall_info, nullptr, false);
  first_state.model_identity = ptmc_state.model_identity;
  bool violation = detsim::plugins::check_initial(first_state);
  violation |= check_assertions(first_state, 0);
  first_state.property_failed = violation;
  first_state.commit_metadata();
  ptmc_state.running_state = first_state;
  for (int node=0; node<NP; ++node) ptmc_state.tracee_image_dirty[node]=false;
  if (violation) throw std::runtime_error("Initial state violates configured properties");
  return 0;
}
