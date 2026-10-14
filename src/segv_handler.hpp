#define _GNU_SOURCE
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <string.h>
#include <ucontext.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <cstdlib>
#include <mutex>
#include <iostream>
#include <thread>
#include <atomic>
#include <array>

// 备用栈大小
constexpr size_t ALT_STACK_SIZE = 128 * 1024;  // 128KB

// 架构寄存器定义（用于记录额外上下文）
#if defined(__x86_64__)
    #define REG_IP REG_RIP
#elif defined(__aarch64__)
    #define REG_IP REG_PC
#else
    #define REG_IP 0
#endif

// 全局安装锁（确保 handler 只装一次）
static std::mutex g_handler_install_mutex;
static std::atomic<bool> g_handler_installed{false};

// 信号安全的输出
static void safe_write(int fd, const char *msg) {
    syscall(SYS_write, fd, msg, strlen(msg));
}

class SegvHandler {
public:
    static void install() {
        std::lock_guard<std::mutex> lock(g_handler_install_mutex);
        if (g_handler_installed.load()) return;

        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = &SegvHandler::handler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigfillset(&sa.sa_mask);  // 阻塞所有其他信号

        if (sigaction(SIGSEGV, &sa, nullptr) != 0) {
            perror("sigaction SIGSEGV failed");
            abort();
        }

        // 也捕获 SIGABRT 和 SIGFPE，便于调试
        sigaction(SIGABRT, &sa, nullptr);
        sigaction(SIGFPE, &sa, nullptr);

        g_handler_installed.store(true);
        safe_write(STDERR_FILENO, "[SegvHandler] Installed\n");
    }

private:
    static void handler(int sig, siginfo_t *info, void *ctx) {
        (void)ctx;

        // 立即输出，不做复杂处理
        safe_write(STDERR_FILENO, "\n=== CRASH SIGNAL ");
        if (sig == SIGSEGV) safe_write(STDERR_FILENO, "SIGSEGV");
        else if (sig == SIGABRT) safe_write(STDERR_FILENO, "SIGABRT");
        else if (sig == SIGFPE) safe_write(STDERR_FILENO, "SIGFPE");
        else safe_write(STDERR_FILENO, "UNKNOWN");
        safe_write(STDERR_FILENO, " ===\n");

        // 获取回溯（最多 128 层）
        void *frames[128];
        int n_frames = backtrace(frames, 128);

        // 输出原始地址
        safe_write(STDERR_FILENO, "Raw backtrace (");
        char buf[16];
        int tmp = n_frames;
        int len = 0;
        while (tmp > 0) { buf[len++] = '0' + (tmp % 10); tmp /= 10; }
        for (int i = len - 1; i >= 0; i--) {
            char c = buf[i];
            syscall(SYS_write, STDERR_FILENO, &c, 1);
        }
        safe_write(STDERR_FILENO, " frames):\n");

        // 使用 backtrace_symbols_fd 输出符号（信号安全，使用 write）
        backtrace_symbols_fd(frames, n_frames, STDERR_FILENO);

        safe_write(STDERR_FILENO, "=== End backtrace ===\n");

        // 使用 _exit 而非 exit，避免死锁
        syscall(SYS_exit_group, 128 + sig);
        __builtin_unreachable();
    }
};

// Thread-Local 备用栈管理
struct ThreadAltStack {
    alignas(16) char stack[ALT_STACK_SIZE];

    ThreadAltStack() {
        // 注意：ss_flags 在 ss_size 之前初始化（符合 struct stack_t 内存布局）
        // struct stack_t { void *ss_sp; int ss_flags; size_t ss_size; };
        stack_t ss;
        ss.ss_sp = stack;
        ss.ss_flags = 0;
        ss.ss_size = ALT_STACK_SIZE;

        if (sigaltstack(&ss, nullptr) != 0) {
            // 诊断：输出错误
            perror("sigaltstack failed");
            safe_write(STDERR_FILENO, "[ThreadAltStack] Failed to set altstack\n");
            abort();
        }

        // 确保 SIGSEGV 未被阻塞（某些线程池会继承阻塞集）
        sigset_t curr_mask;
        pthread_sigmask(SIG_SETMASK, nullptr, &curr_mask);

        if (sigismember(&curr_mask, SIGSEGV)) {
            safe_write(STDERR_FILENO, "[ThreadAltStack] WARNING: SIGSEGV is blocked! Unblocking...\n");
            sigset_t unblock;
            sigemptyset(&unblock);
            sigaddset(&unblock, SIGSEGV);
            pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
        }

        // 安装全局 handler（线程安全）
        SegvHandler::install();

        // 诊断信息
        safe_write(STDOUT_FILENO, "[ThreadAltStack] Altstack set for thread\n");
    }
};
