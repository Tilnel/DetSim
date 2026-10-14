#include "xstate.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <elf.h>
#include <stdexcept>
#include <string>
#include <sys/ptrace.h>
#include <sys/uio.h>

namespace {
constexpr size_t minimum_size = 576; // FXSAVE area + XSAVE header
constexpr size_t maximum_size = 1024 * 1024; // explicit resource bound, not truncation

void validate(const std::vector<uint8_t>& state)
{
    if (state.size() < minimum_size || state.size() > maximum_size)
        throw std::runtime_error("Invalid XSTATE snapshot length");
    // Linux's ptrace ABI uses standard XSAVE layout, never compacted XSAVES.
    for (size_t i = 520; i < 576; ++i)
        if (state[i] != 0)
            throw std::runtime_error("Unsupported compacted/reserved XSTATE header");
}

[[noreturn]] void fail(const char* operation, pid_t tid)
{
    const int error = errno;
    throw std::runtime_error(std::string(operation) + " for TID " +
                             std::to_string(tid) + ": " + std::strerror(error));
}
}

std::vector<uint8_t> capture_xstate(pid_t tid)
{
    // GETREGSET reports min(requested, actual), so an equal size is not proof
    // of completeness. Grow until the kernel returns strictly less than asked.
    for (size_t capacity = 1024; capacity <= maximum_size; capacity *= 2) {
        std::vector<uint8_t> state(capacity, 0);
        iovec io{state.data(), state.size()};
        if (ptrace(PTRACE_GETREGSET, tid, NT_X86_XSTATE, &io) == -1)
            fail("PTRACE_GETREGSET NT_X86_XSTATE", tid);
        if (io.iov_len >= capacity)
            continue;
        state.resize(io.iov_len);
        validate(state);
        // Linux copy_xstate_to_uabi_buf supplies architectural init values for
        // absent components and zeros layout gaps/header padding. Explicitly
        // remove FXSAVE reserved bytes inside the legacy x87 slots as well.
        state[5] = 0;
        for (size_t i = 0; i < 8; ++i)
            std::fill(state.begin() + 32 + i * 16 + 10,
                      state.begin() + 32 + (i + 1) * 16, 0);
        std::fill(state.begin() + 416, state.begin() + 464, 0);
        return state;
    }
    throw std::runtime_error("XSTATE register set exceeds supported size bound");
}

void restore_xstate(pid_t tid, const std::vector<uint8_t>& state)
{
    validate(state);
    // SETREGSET requires the complete kernel-specific layout. Incompatible
    // hosts/feature permissions fail explicitly; snapshots are not portable.
    iovec io{const_cast<uint8_t*>(state.data()), state.size()};
    if (ptrace(PTRACE_SETREGSET, tid, NT_X86_XSTATE, &io) == -1)
        fail("PTRACE_SETREGSET NT_X86_XSTATE", tid);
    if (io.iov_len != state.size())
        throw std::runtime_error("Short XSTATE register restore");
}
