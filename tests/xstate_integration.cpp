#include "core/state/state.h"
#include "core/state/xstate.h"
#include <cereal/archives/binary.hpp>
#include <cereal/types/vector.hpp>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/wait.h>

static void require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

struct Result {
    unsigned long xmm;
    unsigned mxcsr;
    unsigned short control;
    double fp;
    alignas(32) unsigned long ymm[4];
};

static void wait_stop(pid_t pid)
{
    int status;
    require(waitpid(pid, &status, 0) == pid && WIFSTOPPED(status) &&
            WSTOPSIG(status) == SIGTRAP, "tracee did not reach register checkpoint");
}

int main()
{
    pid_t child = -1;
    try {
        const bool avx = __builtin_cpu_supports("avx");
        auto* out = static_cast<Result*>(mmap(nullptr, sizeof(Result), PROT_READ | PROT_WRITE,
                                             MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        require(out != MAP_FAILED, "mmap failed");
        child = fork();
        require(child >= 0, "fork failed");
        if (child == 0) {
            if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr)) _exit(2);
            const unsigned initial_mxcsr = 0x3f80, changed_mxcsr = 0x5f80;
            const unsigned short initial_control = 0x077f, changed_control = 0x0b7f;
            const unsigned long initial_ymm[4] = {1, 2, 3, 4};
            const unsigned long changed_ymm[4] = {5, 6, 7, 8};
            if (avx) asm volatile("vmovdqu %0, %%ymm14" : : "m"(initial_ymm) : "xmm14");
            asm volatile(
                "fninit\n\tfld1\n\tfldcw %3\n\tldmxcsr %1\n\t"
                "mov $11, %%rax\n\tmovq %%rax, %%xmm15\n\tint3\n\t"
                "fninit\n\tfldz\n\tfldcw %4\n\tldmxcsr %2\n\t"
                "mov $22, %%rax\n\tmovq %%rax, %%xmm15\n\t"
                : : "r"(out), "m"(initial_mxcsr), "m"(changed_mxcsr),
                    "m"(initial_control), "m"(changed_control) : "rax", "xmm15", "memory");
            if (avx) asm volatile("vmovdqu %0, %%ymm14" : : "m"(changed_ymm) : "xmm14");
            asm volatile("int3\n\tmovq %%xmm15, %0\n\tstmxcsr %1\n\tfnstcw %2\n\tfstpl %3"
                         : "=m"(out->xmm), "=m"(out->mxcsr), "=m"(out->control), "=m"(out->fp)
                         : : "memory");
            if (avx) asm volatile("vmovdqu %%ymm14, %0" : "=m"(out->ymm));
            _exit(0);
        }
        wait_stop(child);
        thread_state saved;
        saved.xstate = capture_xstate(child);
        require(saved.xstate == capture_xstate(child), "unchanged register capture is unstable");
        std::stringstream archive;
        { cereal::BinaryOutputArchive ar(archive); ar(saved); }
        thread_state reloaded;
        { cereal::BinaryInputArchive ar(archive); ar(reloaded); }
        require(saved.xstate == reloaded.xstate, "thread archive lost XSTATE");
        require(ptrace(PTRACE_CONT, child, nullptr, nullptr) == 0, "continue failed");
        wait_stop(child);
        require(saved.xstate != capture_xstate(child), "mutation did not change state identity bytes");
        restore_xstate(child, reloaded.xstate);
        require(saved.xstate == capture_xstate(child), "restored register capture differs");
        restore_xstate(child, reloaded.xstate);
        require(saved.xstate == capture_xstate(child), "repeated restore is unstable");
        require(ptrace(PTRACE_CONT, child, nullptr, nullptr) == 0, "continue failed");
        int status;
        require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "child failed");
        child = -1;
        require(out->xmm == 11, "XMM15 leaked across branch restore");
        require(out->mxcsr == 0x3f80, "MXCSR restore failed");
        require(out->control == 0x077f && out->fp == 1.0, "x87 restore failed");
        if (avx) {
            for (unsigned i = 0; i < 4; ++i)
                require(out->ymm[i] == i + 1, "AVX upper/lower halves restore failed");
        }
        bool failed = false;
        try { capture_xstate(-1); } catch (const std::runtime_error&) { failed = true; }
        require(failed, "failed capture did not throw");
        failed = false;
        try { restore_xstate(getpid(), {}); } catch (const std::runtime_error&) { failed = true; }
        require(failed, "missing XSTATE did not throw");
        failed = false;
        try { restore_xstate(-1, saved.xstate); } catch (const std::runtime_error&) { failed = true; }
        require(failed, "failed restore did not throw");
        auto invalid = saved.xstate;
        invalid[520] = 1;
        failed = false;
        try { restore_xstate(getpid(), invalid); } catch (const std::runtime_error&) { failed = true; }
        require(failed, "unsupported XSTATE layout did not throw");
        munmap(out, sizeof(Result));
        std::printf("PASS XMM15, MXCSR, x87, archive, identity, repeated restore, errors; AVX %s (%zu-byte XSTATE)\n",
                    avx ? "PASS" : "SKIP (host lacks AVX)", saved.xstate.size());
        return 0;
    } catch (const std::exception& error) {
        if (child > 0) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
