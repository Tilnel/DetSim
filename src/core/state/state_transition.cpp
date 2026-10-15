#include "state_transition.h"
#include <algorithm>
#include "state_view.h"
#include "plugins.h"
#include "common.h"
#include "runtime_profile.h"
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
                                  int process_index)
{
  if (source_state.property_failed)
    throw std::runtime_error("Cannot execute a terminal property violation; load a predecessor state first");
  TransitionResult result;
  ptmc_state.cursor = process_index;

  // restore state to tracees with timing
  auto t1 = std::chrono::high_resolution_clock::now();
  source_state.recover_running_state();
  auto t2 = std::chrono::high_resolution_clock::now();
  result.restore_time =
      std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1);

  // do syscalls - copy per-thread syscall info for each process
  syscall_info new_syscalls[NP][MAX_THREADS_PER_PROCESS];
  for (int i = 0; i < NP; i++)
    memcpy(new_syscalls[i], source_state.child[i].si, sizeof(syscall_info) * MAX_THREADS_PER_PROCESS);

  // Get current thread index for the active process
  int thread_idx = ptmc_state.current_thread_idx[process_index];
  syscall_info executed = new_syscalls[process_index][thread_idx];
  int ckpt = exec_once(source_state, executed);
  // Automatic execution may skip a blocked/exited thread. Attribute the
  // completed syscall to the actor that actually ran, preserving other slots.
  thread_idx = ptmc_state.current_thread_idx[process_index];
  new_syscalls[process_index][thread_idx] = executed;

  // save state with timing
  auto t3 = std::chrono::high_resolution_clock::now();
  result.new_state = sys_state(new_syscalls, &source_state, false);
  if (ckpt != CKPT_DISCARD) {
    detsim_event_v1 event{};
    event.process = process_index; event.thread = thread_idx;
    event.syscall_number = executed.nr;
    event.choice = ptmc_state.n_choose ? ptmc_state.choose : -1;
    std::copy(std::begin(executed.args), std::end(executed.args), event.args);
    event.result = static_cast<int64_t>(executed.rval);
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
                 ptmc_state.n_choose ? ptmc_state.choose : -1);
  ptmc_state.running_state = result.new_state;
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
      kill(child, SIGKILL);
      while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
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

extern std::unordered_map<int, void *> rseq_struct;
extern std::unordered_map<int, int> rseq_len;

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

  /* Handle clone: add CLONE_PTRACE flag to auto-trace child threads */
  if (ret.nr == SYS_clone) {
    uint64_t new_flags = ret.args[0] | 0x00002000; /* CLONE_PTRACE */
    tracee_set_rdi(pid, new_flags);
    LOG_INFO("Modified clone flags to add CLONE_PTRACE");
  }

  /* Handle clone3: modify clone_args.flags to add CLONE_PTRACE */
  if (ret.nr == SYS_clone3) {
    uint64_t args_ptr = ret.args[0];
    if (args_ptr != 0) {
      // Read flags from clone_args (first field)
      uint64_t flags = ptrace(PTRACE_PEEKDATA, pid, args_ptr, NULL);
      // Add CLONE_PTRACE flag
      flags |= 0x00002000; /* CLONE_PTRACE */
      ptrace(PTRACE_POKEDATA, pid, args_ptr, flags);
      LOG_INFO("Modified clone3 args at %p, flags=0x%lx", (void*)args_ptr, flags);
    }
  }

  /* exit */
  resume_startup_syscall(pid);

  ptrace_right(PTRACE_GET_SYSCALL_INFO, pid, (void *)sizeof(info), &info);
  if (info.op != PTRACE_SYSCALL_INFO_EXIT)
    throw std::runtime_error("Unexpected syscall phase before configured RuntimeProfile startup boundary");
  ret.rval = info.exit.rval;

  /* eliminate indeterministic syscall result */
  /* set_tid_address */
  if (ret.nr == SYS_set_tid_address)
  {
    ret.rval = 114514;
    tracee_set_rax(pid, 114514);
    LOG_INFO("Neutralize set_tid_address, return 114514");
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
  /* rseq mask */
  if (ret.nr == SYS_rseq)
  {
    rseq_struct[pid] = (void *)ret.args[0];
    rseq_len[pid] = ret.args[1];
    LOG_INFO("Record rseq(%p, %d)", (void *)ret.args[0], ret.args[1]);
  }

  if (ret.nr == SYS_getcpu && ret.rval == 0)
  {
    const unsigned int zero = 0;
    for (int argument = 0; argument < 2; ++argument)
      if (ret.args[argument] &&
          !guest_write_exact(pid, reinterpret_cast<void*>(ret.args[argument]), &zero, sizeof(zero)))
        throw std::runtime_error("Cannot normalize startup getcpu output");
  }

  /* Record new thread creation from clone/clone3 */
  if ((ret.nr == SYS_clone || ret.nr == SYS_clone3) && ret.rval > 0)
  {
    pid_t new_tid = (pid_t)ret.rval;
    LOG_INFO("New thread created: TID %d from parent %d", new_tid, pid);

    // Record thread creation in tracee_state
    int tracee_idx = ptmc_state.cursor;
    if (tracee_idx >= 0 && tracee_idx < NP)
    {
      auto &child_state = ptmc_state.running_state.child[tracee_idx];

      // Get clone flags from arguments
      uint64_t clone_flags = 0;
      uint64_t stack_addr = 0;
      if (ret.nr == SYS_clone)
      {
        clone_flags = ret.args[0];
        stack_addr = ret.args[1];
      }
      else if (ret.nr == SYS_clone3)
      {
        uint64_t args_ptr = ret.args[0];
        if (args_ptr != 0)
        {
          clone_flags = ptrace(PTRACE_PEEKDATA, pid, args_ptr, NULL);
          // stack is at offset 5*8 = 40 bytes in clone_args (after flags, pidfd, child_tid, parent_tid, exit_signal)
          stack_addr = ptrace(PTRACE_PEEKDATA, pid, args_ptr + 40, NULL);
        }
      }

      // Add thread to state
      child_state.add_thread(new_tid, clone_flags, stack_addr, pid, false);

      LOG_INFO("Recorded thread %d in tracee %d state (flags=0x%lx)",
               new_tid, tracee_idx, clone_flags);
    }

    // Wait for the new thread to stop (SIGSTOP from CLONE_PTRACE)
    // This is crucial: the new thread is created in a stopped state, and we
    // need to wait for it and continue it so it can initialize and signal
    // the parent thread (which may be waiting on a futex)
    int wstatus_new;
    int wait_retries = 0;
    pid_t waited = 0;

    // Try non-blocking wait first, then blocking wait with timeout
    while (wait_retries < 100 && waited == 0) {
      waited = waitpid(new_tid, &wstatus_new, WNOHANG);
      if (waited == 0) {
        usleep(1000);  // 1ms delay between retries
        wait_retries++;
      }
    }

    // If still not ready, try blocking wait as last resort
    if (waited == 0) {
      waited = waitpid(new_tid, &wstatus_new, WNOHANG);
    }

    if (waited == new_tid && WIFSTOPPED(wstatus_new))
    {
      LOG_INFO("New thread %d stopped with signal %d after %d retries, continuing",
               new_tid, WSTOPSIG(wstatus_new), wait_retries);

      // Continue the new thread to its first syscall entry
      // This is essential: the new thread must run to initialize itself
      // and signal any waiting threads (via futex wake)
      ptrace_right(PTRACE_SYSCALL, new_tid, NULL, NULL);

      // Note: We don't wait here because the parent thread also needs to run.
      // The new thread will hit a syscall entry and stop, then when the user
      // switches to it with 'thread N', it will be ready to execute.
    }
    else
    {
      LOG_WARN("New thread %d not in expected stopped state (waited=%d, retries=%d)",
               new_tid, waited, wait_retries);
    }
  }

  return ret;
}

static void init_tracee_state(int index)
{
  int pid = pids[index];
  int wstatus = 0;
  const auto& profile = detsim::runtime_profile();
  const int stop_nr = detsim::startup_syscall_for_node(index);
  LOG_INFO("Startup prelude boundary: syscall %d (limit %zu)",
           stop_nr, profile.startup_syscall_limit);
  if (waitpid(pid, &wstatus, 0) != pid || !WIFSTOPPED(wstatus) || WSTOPSIG(wstatus) != SIGSTOP)
    throw std::runtime_error("Tracee failed to reach initial startup stop");
  /* tracee: stop at raise(SIGSTOP) */

  /* ptrace_right(PTRACE_SEIZE, pid, 0, 0); */
  ptrace_right(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACESYSGOOD);

  /* skip execve */
  while (extract_one_syscall(pid).nr != SYS_execve)
    ;

  /* execve(2): after a successful execve, a SIGTRAP *
   * (instead of SIGTRAP | 0x80) will be sent to it  */
  ptrace_right(PTRACE_SYSCALL, pid, 0, 0);
  if (waitpid(pid, &wstatus, 0) != pid || !WIFSTOPPED(wstatus) || WSTOPSIG(wstatus) != SIGTRAP)
    throw std::runtime_error("Tracee failed to exec before configured RuntimeProfile startup boundary");

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

  execv(ptmc_state.tracee[id].executable, ptmc_state.tracee[id].argv);

  perror("exec");
}

int state_initialization()
{
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

  pids[0] = fork();
  for (int i = 0; i < NP - 1; i++)
  {
    if (pids[i])
      pids[i + 1] = fork();
    else
    {
      // setpgid(0, pgid);
      start_tracee(i);
    }
  }

  if (!pids[NP - 1])
    start_tracee(NP - 1);

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
  if (violation) throw std::runtime_error("Initial state violates configured properties");
  return 0;
}
