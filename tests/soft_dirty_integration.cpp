#include "../src/core/state/soft_dirty_experiment.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

int main() {
    constexpr size_t page = 4096;
    constexpr size_t extent = 512*page;
    int commands[2]{};
    pid_t child = -1;
    try {
        require(sysconf(_SC_PAGESIZE) == long(page), "fixture needs 4KiB pages");
        require(pipe(commands) == 0, "pipe");
        auto* memory = static_cast<unsigned char*>(mmap(nullptr, extent,
            PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0));
        require(memory != MAP_FAILED, "mmap");
        memset(memory, 17, extent);
        child = fork(); require(child >= 0, "fork");
        if (!child) {
            close(commands[1]);
            if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr)) _exit(2);
            raise(SIGSTOP);
            for (;;) {
                unsigned char command;
                if (read(commands[0], &command, 1) != 1) _exit(3);
                switch (command) {
                case 1: memory[page+13]++; break;
                case 2: if (read(commands[0], memory+2*page+19, 1) != 1) _exit(4); break;
                case 3: if (madvise(memory+3*page, page, MADV_DONTNEED)) _exit(5); break;
                case 4:
                    if (munmap(memory+4*page, page)) _exit(6);
                    if (mmap(memory+4*page, page, PROT_READ|PROT_WRITE,
                        MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) == MAP_FAILED) _exit(7);
                    break;
                case 5: if (mprotect(memory+5*page, page, PROT_READ)) _exit(8); break;
                case 6: memory[6*page+3]++; memory[8*page-5]++; break;
                case 7: memory[300*page+13]++; break;
                default: break;
                }
                raise(SIGSTOP);
            }
        }
        close(commands[0]);
        int status;
        auto stopped = [&] {
            require(waitpid(child, &status, 0) == child && WIFSTOPPED(status)
                    && WSTOPSIG(status) == SIGSTOP, "child stop");
        };
        stopped();
        require(ptrace(PTRACE_SETOPTIONS, child, nullptr, PTRACE_O_EXITKILL) == 0, "exitkill");
        using namespace detsim::experiment;
        SoftDirtyCapture capture(Mode::verify);
        std::vector<unsigned char> bytes(extent);
        auto snapshot = [&] {
            capture.capture(child, {{reinterpret_cast<uintptr_t>(memory), bytes.size(), bytes.data()}}, "fixed-address");
        };
        auto run = [&](unsigned char command, bool payload = false) {
            require(write(commands[1], &command, 1) == 1, "send command");
            if (payload) { unsigned char value = 93; require(write(commands[1], &value, 1) == 1, "payload"); }
            require(ptrace(PTRACE_CONT, child, nullptr, nullptr) == 0, "continue");
            stopped();
        };
        snapshot(); const auto first_bytes = capture.stats().bytes_read; snapshot();
        require(capture.stats().bytes_read == first_bytes, "unchanged memory reread");
        run(1); snapshot(); require(bytes[page+13] == 18, "user write");
        run(7); snapshot(); require(bytes[300*page+13] == 18, "pagemap batch boundary");
        run(2, true); snapshot(); require(bytes[2*page+19] == 93, "kernel read copyout");
        unsigned char value = 71;
        iovec local{&value, 1}, remote{memory+8*page+3, 1};
        require(process_vm_writev(child, &local, 1, &remote, 1, 0) == 1, "external process_vm write");
        snapshot(); require(bytes[8*page+3] == 71, "external write captured");
        run(3); snapshot(); require(bytes[3*page] == 0, "discard captured");
        run(4); snapshot(); require(bytes[4*page] == 0, "replacement captured");
        run(5); snapshot();
        require(ptrace(PTRACE_POKEDATA, child, memory+5*page+16, 99) == 0, "readonly ptrace write");
        snapshot(); require(bytes[5*page+16] == 99, "readonly write captured");
        // A restore-like bulk write must remain within the tracking epoch.
        std::array<unsigned char, page> restored; restored.fill(42);
        local = {restored.data(), restored.size()}; remote = {memory+9*page, page};
        require(process_vm_writev(child, &local, 1, &remote, 1, 0) == long(page), "restore-like write");
        snapshot(); require(bytes[9*page] == 42 && bytes[10*page-1] == 42, "restore captured");
        // clear_refs is process-global: use one tracker even across layout changes.
        std::array<unsigned char, 2*page-7> tail;
        auto partial_snapshot = [&] {
            capture.capture(child, {{reinterpret_cast<uintptr_t>(memory+6*page+3), tail.size(), tail.data()}}, "partial");
        };
        const auto fallbacks = capture.stats().full_fallbacks;
        partial_snapshot(); run(6); partial_snapshot();
        require(capture.stats().full_fallbacks == fallbacks+1, "layout fallback");
        require(tail.front() == 18 && tail.back() == 18, "partial page boundaries");
        std::array<unsigned char, page> first, second;
        auto multi_snapshot = [&] {
            capture.capture(child, {{reinterpret_cast<uintptr_t>(memory), first.size(), first.data()},
                {reinterpret_cast<uintptr_t>(memory+300*page), second.size(), second.data()}}, "two regions");
        };
        multi_snapshot(); run(7); multi_snapshot();
        require(first[13] == 17 && second[13] == 19, "multiple regions");
        capture.report();
        // Prove the oracle rejects a missed change if an outsider clears our epoch.
        run(7);
        int clear = open(("/proc/"+std::to_string(child)+"/clear_refs").c_str(), O_WRONLY);
        require(clear >= 0 && write(clear, "4", 1) == 1, "injected epoch loss");
        close(clear);
        bool rejected = false;
        try { multi_snapshot(); }
        catch (const std::runtime_error& error) {
            rejected = std::string(error.what()).find("missed changed raw page") != std::string::npos;
        }
        require(rejected && capture.stats().missed_pages == 1, "oracle accepted lost dirty mark");
        kill(child, SIGKILL); waitpid(child, nullptr, 0); child = -1;
        close(commands[1]); munmap(memory, extent);
        puts("PASS soft-dirty: user/kernel/external writes, readonly ptrace, discard, replacement, restore and partial pages");
        return 0;
    } catch (const std::exception& error) {
        if (child > 0) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
        fprintf(stderr, "FAIL soft-dirty: %s\n", error.what());
        return 1;
    }
}
