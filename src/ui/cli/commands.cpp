/*
 * commands.cpp - CLI command handlers
 */

#include "commands.h"
#include "scheduler.h"
#include "state/state.h"
#include "state/state_store.h"
#include "state/sysstate_store.h"
#include "monitor.h"
#include "guest.h"
#include "fs/fsstate.h"
#include "net/sockstate.h"
#include "log_wrapper.h"
#include "ncurses_ui.h"
#include "utils.h"
#include <readline/readline.h>
#include <readline/history.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <sstream>
#include <arpa/inet.h>
#include <netinet/in.h>

/* Defined in expr.cpp */
long expr(const char *e, bool *success);
void expr_print(const char *e);

/* Defined in dwarf.cpp */
void dwarf_print_stack_trace(int pid);
bool dwarf_set_frame(int frame_id);
int dwarf_get_current_frame(void);
void dwarf_print_local_vars(int pid);

/* Defined in config.cpp */
extern int auto_mode;
extern char *cfg_file;
extern char *log_file;

/* ======================================================================
 * Command Table
 * ====================================================================== */

static CommandEntry cmd_table[] = {
    {"help", "Display information about all supported commands", cmd_help},
    {"bfs", "Perform breadth-first search [--quiet]", cmd_bfs},
    {"dfs", "Perform depth-first search <depth> [--quiet]", cmd_dfs},
    {"rand", "Perform random search <depth> [--quiet]", cmd_rand},
    {"q", "Exit ptraceMC", cmd_q},
    {"si", "Step from current state, on focused process", cmd_si},
    {"sw", "Switch control focus on the n-th process", cmd_sw},
    {"thread", "Switch to thread N in current process (thread to list)", cmd_thread},
    {"load", "Load state of given StateHash", cmd_load},
    {"info", "Display state history (info current: scalar current edge only)", cmd_info},
    {"batch", "Read command list from file", cmd_batch},
    {"p", "Calculate expression", cmd_p},
    {"x", "Examine memory: x/[count][format][size] <addr>", cmd_x},
    {"bt", "Print backtrace with arguments", cmd_bt},
    {"frame", "Select stack frame", cmd_frame},
    {"locals", "Show local variables in current frame", cmd_locals},
    {"ls", "List files in current node's filesystem", cmd_ls},
    {"stat", "Show stat of files matching pattern", cmd_stat},
    {"hexdump", "Hexdump of a file (c: continue, q: quit)", cmd_hexdump},
    {"diff", "Compare two system states by hash prefix", cmd_diff},
};

const CommandEntry* get_command_table() {
    return cmd_table;
}

size_t get_command_count() {
    return sizeof(cmd_table) / sizeof(cmd_table[0]);
}

const CommandEntry* find_command(const char *name) {
    for (size_t i = 0; i < get_command_count(); i++) {
        if (strcmp(name, cmd_table[i].name) == 0) {
            return &cmd_table[i];
        }
    }
    return nullptr;
}

/* ======================================================================
 * Readline Interface
 * ====================================================================== */

static char *line_read = NULL;

char* read_line(const char *prompt) {
    if (line_read) {
        free(line_read);
        line_read = NULL;
    }

    line_read = readline(prompt);

    if (line_read && *line_read) {
        add_history(line_read);
    }

    return line_read;
}

void cleanup_readline() {
    if (line_read) {
        free(line_read);
        line_read = NULL;
    }
    clear_history();
}

/* ======================================================================
 * Command Parsing and Execution
 * ====================================================================== */

int execute_command_line(char *cmd_line) {
    char *str_end = cmd_line + strlen(cmd_line);
    char *cmd = strtok(cmd_line, " \t\r\n");
    if (cmd == NULL) {
        return 0;
    }

    const CommandEntry *entry = find_command(cmd);
    if (entry == nullptr) {
        detsim::ui::ui_printf("Unknown command '%s'\n", cmd);
        return 0;
    }

    char *args = cmd + strlen(cmd) + 1;
    if (args >= str_end) {
        args = nullptr;
    } else {
        while (*args && isspace(static_cast<unsigned char>(*args))) args++;
        if (*args == '\0') args = nullptr;
    }

    return entry->handler(args);
}

/* ======================================================================
 * Basic Commands
 * ====================================================================== */

void show_welcome() {
    detsim::ui::ui_printf("Build time: %s, %s\n", __TIME__, __DATE__);
    detsim::ui::ui_printf("Welcome to ptraceMC!\n");
    detsim::ui::ui_printf("For help, type \"help\"\n");
}

int cmd_help(char *args) {
    if (args == NULL || *args == '\0') {
        for (size_t i = 0; i < get_command_count(); i++) {
            detsim::ui::ui_printf("%s - %s\n", cmd_table[i].name, cmd_table[i].description);
        }
    } else {
        const CommandEntry *entry = find_command(args);
        if (entry) {
            detsim::ui::ui_printf("%s - %s\n", entry->name, entry->description);
        } else {
            detsim::ui::ui_printf("Unknown command '%s'\n", args);
        }
    }
    return 0;
}

int cmd_q(char *args) {
    (void)args;
    ptmc_state.state = PTMC_QUIT;
    detsim::ui::request_ui_exit();
    return -1;
}

/* ======================================================================
 * Execution Control Commands
 * ====================================================================== */

int cmd_si(char *args) {
    int cursor = ptmc_state.cursor;
    if (cursor < 0 || cursor >= NP) {
        detsim::ui::ui_printf("cursor = %d, out of range [0, %d). Please set again.\n",
                              cursor, NP);
        return 0;
    }

    if (args && args[0]) {
        int choice = atoi(args);
        if (choice >= 0) {
            ptmc_state.batch_choice_preset = choice;
        }
    }

    int ret = load_exec_store();
    if (ret == 0) {
        show_syscall(ptmc_state.pids[cursor], ptmc_state.running_state.ts_hash[cursor]);
    }
    return 0;
}

int cmd_sw(char *args) {
    if (args == NULL) {
        detsim::ui::ui_printf("Please provide a process number\n");
        return 0;
    }
    int proc = atoi(args);
    if (proc < 0 || proc >= NP) {
        detsim::ui::ui_printf("Invalid process number %d (must be 0-%d)\n", proc, NP - 1);
        return 0;
    }
    ptmc_state.cursor = proc;
    detsim::ui::ui_printf("Switched to process %d (pid=%d)\n", proc, ptmc_state.pids[proc]);
    return 0;
}

int cmd_thread(char *args) {
    int tracee_idx = ptmc_state.cursor;
    if (tracee_idx < 0 || tracee_idx >= NP) {
        detsim::ui::ui_printf("No process selected\n");
        return 0;
    }

    auto& threads = ptmc_state.running_state.child[tracee_idx].threads;

    if (args == NULL || strlen(args) == 0) {
        int current = ptmc_state.current_thread_idx[tracee_idx];
        detsim::ui::ui_printf("Threads in process %d (pid=%d):\n",
                              tracee_idx, ptmc_state.pids[tracee_idx]);
        if (threads.empty()) {
            detsim::ui::ui_printf("  (single-threaded, main thread only) state=runnable\n");
        } else {
            for (size_t i = 0; i < threads.size(); i++) {
                const char* marker = (i == (size_t)current) ? "*" : " ";
                const auto& thread = threads[i];
                const char* state = !thread.alive ? "exited" :
                    thread.pending_syscall && thread.pending_syscall->phase == PendingPhase::Waiting
                        ? "waiting" : "runnable";
                detsim::ui::ui_printf("%s [%zu] TID %d state=%s %s\n",
                                      marker, i, thread.tid, state,
                                      threads[i].is_main ? "(main)" : "");
            }
        }
        return 0;
    }

    int thread_idx = atoi(args);
    if (thread_idx < 0 || thread_idx >= (int)threads.size()) {
        detsim::ui::ui_printf("Invalid thread index %d (must be 0-%zu)\n",
                              thread_idx, threads.size() - 1);
        return 0;
    }

    ptmc_state.current_thread_idx[tracee_idx] = thread_idx;
    // Also update the tracee_state so that execute_step syncs correctly
    ptmc_state.running_state.child[tracee_idx].current_thread_idx = thread_idx;
    detsim::ui::ui_printf("Switched to thread %d (TID=%d) in process %d\n",
                          thread_idx, threads[thread_idx].tid, tracee_idx);
    return 0;
}

int cmd_load(char *args) {
    if (args == NULL) {
        detsim::ui::ui_printf("Usage: load <hash_prefix>\n");
        return 1;
    }

    char *arg = strtok(args, " ");
    if (arg == NULL) {
        detsim::ui::ui_printf("No Arguments!\n");
        return 1;
    }

    SysStateStore &store = SysStateStore::instance();
    std::vector<hash_type> matches = store.find_by_prefix(arg);

    if (matches.size() == 1) {
        sys_state state(matches[0]);
        state.recover_running_state();
        ptmc_state.running_state = state;
        detsim::ui::ui_printf("Loaded state %016lx\n", matches[0]);
    } else if (matches.size() == 0) {
        detsim::ui::ui_printf("No state found with prefix '%s'\n", arg);
    } else {
        detsim::ui::ui_printf("Multiple states match prefix '%s':\n", arg);
        for (size_t i = 0; i < matches.size() && i < 10; i++) {
            detsim::ui::ui_printf("  %016lx\n", matches[i]);
        }
        if (matches.size() > 10) {
            detsim::ui::ui_printf("  ... and %zu more\n", matches.size() - 10);
        }
    }
    return 0;
}

/* ======================================================================
 * Exploration Commands
 * ====================================================================== */

namespace {
// Display policy belongs to the command, including every scheduler early return.
struct SearchDisplayScope {
    int previous_auto = auto_mode;
    bool previous_history = syscall_history_enabled;
    explicit SearchDisplayScope(bool quiet) {
        auto_mode = 1;
        syscall_history_enabled = !quiet;
    }
    ~SearchDisplayScope() {
        auto_mode = previous_auto;
        syscall_history_enabled = previous_history;
    }
};

bool parse_search_args(char* args, bool needs_depth, int& depth, bool& quiet) {
    std::istringstream input(args ? args : "");
    std::string token;
    if (needs_depth) {
        if (!(input >> depth) || depth <= 0) return false;
    }
    if (input >> token) {
        if (token != "--quiet") return false;
        quiet = true;
        if (input >> token) return false;
    }
    return true;
}
}

int cmd_bfs(char *args) {
    int depth = 0; bool quiet = false;
    if (!parse_search_args(args, false, depth, quiet)) {
        detsim::ui::ui_printf("Usage: bfs [--quiet]\n");
        return 1;
    }
    SearchDisplayScope display(quiet);
    exec_bfs();
    return 0;
}

int cmd_dfs(char *args) {
    int depth = 0; bool quiet = false;
    if (!parse_search_args(args, true, depth, quiet)) {
        detsim::ui::ui_printf("Usage: dfs <depth> [--quiet]\n");
        return 1;
    }
    SearchDisplayScope display(quiet);
    return exec_dfs(depth);
}

int cmd_rand(char *args) {
    int depth = 0; bool quiet = false;
    if (!parse_search_args(args, true, depth, quiet)) {
        detsim::ui::ui_printf("Usage: rand <depth> [--quiet]\n");
        return 1;
    }
    SearchDisplayScope display(quiet);
    return exec_rand(depth);
}

/* ======================================================================
 * Expression and Debug Commands
 * ====================================================================== */

int cmd_p(char *args) {
    if (!args || (args[0] == 0)) {
        detsim::ui::ui_printf("Give an expression\n");
        return 1;
    }
    expr_print(args);
    return 0;
}

int cmd_bt(char *args) {
    (void)args;
    int pid = ptmc_state.pids[ptmc_state.cursor];
    dwarf_print_stack_trace(pid);
    return 0;
}

int cmd_frame(char *args) {
    if (!args) {
        detsim::ui::ui_printf("Current frame: %d\n", dwarf_get_current_frame());
        return 0;
    }
    int frame = atoi(args);
    if (dwarf_set_frame(frame)) {
        detsim::ui::ui_printf("Switched to frame %d\n", frame);
    } else {
        detsim::ui::ui_printf("Invalid frame %d\n", frame);
    }
    return 0;
}

int cmd_locals(char *args) {
    (void)args;
    int pid = ptmc_state.pids[ptmc_state.cursor];
    dwarf_print_local_vars(pid);
    return 0;
}

/* ======================================================================
 * Memory Commands
 * ====================================================================== */

int cmd_x(char *args) {
    if (!args) {
        detsim::ui::ui_printf("Usage: x/[count][format][size] <addr>\n");
        return 0;
    }

    int count = 4;
    char format = 'x';
    int size = 1;
    uint64_t addr = 0;

    if (args[0] == '/') {
        args++;
        if (isdigit(args[0])) {
            count = atoi(args);
            while (isdigit(*args)) args++;
        }
        if (*args && strchr("xd uotcsi", *args)) {
            format = *args;
            args++;
        }
        if (*args) {
            switch (*args) {
                case 'b': size = 1; break;
                case 'h': size = 2; break;
                case 'w': size = 4; break;
                case 'g': size = 8; break;
            }
            args++;
        }
        while (*args && isspace(*args)) args++;
    }

    addr = strtoul(args, NULL, 0);
    int pid = ptmc_state.pids[ptmc_state.cursor];

    detsim::ui::ui_printf("Memory at 0x%lx:\n", addr);

    for (int i = 0; i < count; i++) {
        if (i % 4 == 0) {
            if (i > 0) detsim::ui::ui_printf("\n");
            detsim::ui::ui_printf("  0x%016lx: ", addr + i * size);
        }

        uint64_t val = 0;
        switch (size) {
            case 1: val = tracee_read_byte(pid, (void *)(addr + i)); break;
            case 2: val = tracee_read_word(pid, (void *)(addr + i * 2)) & 0xFFFF; break;
            case 4: val = tracee_read_word(pid, (void *)(addr + i * 4)) & 0xFFFFFFFF; break;
            case 8: val = tracee_read_word(pid, (void *)(addr + i * 8)); break;
        }

        switch (format) {
            case 'x': detsim::ui::ui_printf("%0*lx ", size * 2, val); break;
            case 'd': detsim::ui::ui_printf("%*ld ", size * 3, (int64_t)val); break;
            case 'u': detsim::ui::ui_printf("%*lu ", size * 3, val); break;
            case 'o': detsim::ui::ui_printf("%0*lo ", size * 3, val); break;
            default:  detsim::ui::ui_printf("%0*lx ", size * 2, val); break;
        }
    }
    detsim::ui::ui_printf("\n");
    return 0;
}

/* ======================================================================
 * Info and State Commands
 * ====================================================================== */

static std::string format_addr_short(const struct sockaddr_in &addr) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    char buf[64];
    snprintf(buf, sizeof(buf), "%s:%d", ip, ntohs(addr.sin_port));
    return std::string(buf);
}

int cmd_info(char *args) {
    if (args && strcmp(args, "current") == 0) {
        show_syscall_current();
        return 0;
    }
    if (args && (strcmp(args, "sock") == 0 || strcmp(args, "socket") == 0)) {
        detsim::ui::ui_printf("[Socket States]\n");
        for (int i = 0; i < NP; i++) {
            const auto &sockets = ptmc_state.sock_states[i].sockets();
            if (!sockets.empty()) {
                detsim::ui::ui_printf("Process %d:\n", i);
                for (const auto &kv : sockets) {
                    const Socket &sock = kv.second;
                    detsim::ui::ui_printf("  fd=%d: type=%d\n", sock.fd, sock.type);
                }
            }
        }
        return 0;
    }

    detsim::ui::ui_printf("Current process: %d (pid=%d)\n",
                          ptmc_state.cursor, ptmc_state.pids[ptmc_state.cursor]);
    show_syscall_history();
    return 0;
}

int cmd_diff(char *args) {
    (void)args;
    detsim::ui::ui_printf("diff: not yet implemented in CLI module\n");
    return 0;
}

/* ======================================================================
 * Filesystem Commands
 * ====================================================================== */

int cmd_ls(char *args) {
    (void)args;
    int tracee_idx = ptmc_state.cursor;
    auto &fs = ptmc_state.fs_states[tracee_idx];

    detsim::ui::ui_printf("Files in VFS for process %d:\n", tracee_idx);
    for (const auto &kv : fs.filesystem) {
        detsim::ui::ui_printf("  %s\n", kv.first.c_str());
    }
    return 0;
}

int cmd_stat(char *args) {
    (void)args;
    detsim::ui::ui_printf("stat: not yet implemented\n");
    return 0;
}

int cmd_hexdump(char *args) {
    (void)args;
    detsim::ui::ui_printf("hexdump: not yet implemented\n");
    return 0;
}

/* ======================================================================
 * Batch Commands
 * ====================================================================== */

int cmd_batch(char *args) {
    static int in_call = 0;
    if (in_call) {
        detsim::ui::ui_printf("cmd_batch is not designed for nested invoke\n");
        return 1;
    }
    in_call = 1;

    if (args == NULL) {
        detsim::ui::ui_printf("Usage: batch <filename>\n");
        in_call = 0;
        return 1;
    }

    FILE *fp = fopen(args, "r");
    if (!fp) {
        detsim::ui::ui_printf("Cannot open file: %s\n", args);
        in_call = 0;
        return 1;
    }

    char line[1024];
    int cmd_count = 0;

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        char *p = line;
        while (*p && isspace(*p)) p++;
        if (*p == '\0' || *p == '#') continue;

        int ret = execute_command_line(p);
        cmd_count++;

        if (ret < 0) break;
    }

    fclose(fp);
    in_call = 0;

    detsim::ui::ui_printf("Batch executed %d commands from %s\n", cmd_count, args);
    return 0;
}
