#include "state/state.h"
#include "state/state_store.h"
#include "commands.h"
#include "monitor.h"
#include "proc_status.h"
#include "ncurses_ui.h"
#include <cstdio>
#include <stdexcept>
#include <sys/syscall.h>
#include <unistd.h>

extern "C" detsim::ui::NCursesUI* get_ncurses_ui() { return nullptr; }
extern TSS state_tree;
static void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
extern int auto_mode;
static bool search_quiet = false, search_throws = false;
static int search_stub() {
  require(auto_mode == 1 && syscall_history_enabled != search_quiet, "search display policy");
  if (search_throws) throw std::runtime_error("injected scheduler failure");
  return 17;
}
extern "C" int __wrap__Z8exec_bfsv() { return search_stub(); }
extern "C" int __wrap__Z8exec_dfsi(int) { return search_stub(); }
extern "C" int __wrap__Z9exec_randi(int) { return search_stub(); }
static std::string current() {
  FILE* output = tmpfile();
  require(output, "temporary output");
  fflush(stdout);
  const int saved = dup(STDOUT_FILENO);
  dup2(fileno(output), STDOUT_FILENO);
  char args[] = "current";
  cmd_info(args);
  fflush(stdout);
  dup2(saved, STDOUT_FILENO); close(saved);
  rewind(output);
  std::string text;
  char buffer[1024];
  while (size_t n = fread(buffer, 1, sizeof(buffer), output)) text.append(buffer, n);
  fclose(output);
  return text;
}
int main() {
  auto& state = ptmc_state.running_state;
  state.ss_hash = 0x22;
  state_tree.clear();
  const auto loads = StateStore::instance().get_stats().load_requests;
  require(current().find("no predecessor") != std::string::npos, "root has no fabricated edge");
  auto& child = state.child[0];
  child.current_thread_idx = 1;
  child.threads.resize(2);
  child.threads[1].tid = 2;
  child.threads[1].alive = true;
  auto& call = child.si[1];
  call = {}; call.nr = SYS_write; call.args[0] = 9; call.args[1] = 1; call.args[2] = 4096;
  call.rval = uintptr_t(-14);
  sys_state predecessor; predecessor.ss_hash = 0x11;
  state_tree_add(predecessor, state, 0, -1);
  child.threads[0].tid = 1;
  child.si[0] = {}; child.si[0].nr = SYS_getpid;
  ptmc_state.cursor = 0;
  char selected[] = "0";
  cmd_thread(selected);
  require(current().find("[TID:2] write(") != std::string::npos,
          "current edge keeps recorded actor after selecting another thread");
  require(current().find("= -14") != std::string::npos, "signed scalar return");
  std::get<3>(state_tree.at(state.ss_hash)) = -1;
  require(current().find("<no syscall metadata>") != std::string::npos, "missing actor is explicit");
  std::get<3>(state_tree.at(state.ss_hash)) = 1;
  auto& pending = child.threads[1].pending_syscall;
  pending.emplace(); pending->nr = SYS_read; pending->args[0] = 7;
  for (auto phase : {PendingPhase::Waiting, PendingPhase::Retry, PendingPhase::Ready}) {
    pending->phase = phase;
    const auto text = current();
    const char* name = phase == PendingPhase::Waiting ? "waiting" : phase == PendingPhase::Retry ? "retry" : "ready";
    require(text.find(std::string("<pending:") + name + ">") != std::string::npos &&
            text.find("read(") != std::string::npos && text.find(" = ") == std::string::npos,
            "pending has original syscall and no guest return");
  }
  pending.reset(); call.nr = SYS_exit_group; call.args[0] = 0;
  state.status[0] = dstatus_exit(0); child.threads[1].alive = false;
  require(current().find("<nonreturning>") != std::string::npos, "exit has no return");
  state.status[0] = dstatus_crash(11);
  const auto fatal = current();
  require(fatal.find("<fatal signal 11>") != std::string::npos && fatal.find("exit_group") == std::string::npos,
          "fatal stop does not misreport preceding syscall");
  require(StateStore::instance().get_stats().load_requests == loads, "current must not read snapshots");
  require(child.current_thread_idx == 0 && ptmc_state.current_thread_idx[0] == 0 && call.nr == SYS_exit_group && !pending,
          "observation does not mutate selected thread or syscall metadata");
  for (auto handler : {cmd_bfs, cmd_dfs, cmd_rand}) {
    for (bool quiet : {false, true}) {
      for (bool throws : {false, true}) {
        search_quiet = quiet; search_throws = throws;
        const std::string arguments = std::string(handler == cmd_bfs ? "" : "12 ") + (quiet ? "--quiet" : "");
        std::vector<char> args(arguments.begin(), arguments.end()); args.push_back('\0');
        auto_mode = 7; syscall_history_enabled = true;
        bool caught = false;
        try { require(handler(args.data()) == (handler == cmd_bfs ? 0 : 17), "scheduler result preserved"); }
        catch (const std::runtime_error& error) {
          require(std::string(error.what()) == "injected scheduler failure", "unexpected command failure");
          caught = true;
        }
        require(caught == throws, "scheduler exception preserved");
        require(auto_mode == 7 && syscall_history_enabled, "search scope restores display/auto mode");
      }
    }
  }
  puts("PASS: scalar current edge without loads/mutation; normal/quiet search scope and exception restoration");
}
