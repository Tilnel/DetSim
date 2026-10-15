#ifndef DETSIM_PROCESS_STATE_H
#define DETSIM_PROCESS_STATE_H

#include <cstdint>
#include <array>
#include <map>

struct syscall_info;

// Linux x86-64 rt_sigaction kernel ABI, not glibc's larger struct sigaction.
struct KernelSigaction {
    uint64_t handler = 0;
    uint64_t flags = 0;
    uint64_t restorer = 0;
    uint64_t mask = 0;
    bool operator==(const KernelSigaction&) const = default;
};
static_assert(sizeof(KernelSigaction)==32);

// Linux x86-64 rlimit64 ABI. Infinity is UINT64_MAX.
struct KernelResourceLimit {
    uint64_t current = 0;
    uint64_t maximum = 0;
    bool operator==(const KernelResourceLimit&) const = default;
};
static_assert(sizeof(KernelResourceLimit)==16);

class ProcessKernelState {
public:
    // A fixed modeled stream, not exhaustive exploration of random outcomes.
    uint64_t random_state = 0x4d595df4d0f33173ULL;
    std::map<int, KernelSigaction> signal_actions;
    std::array<KernelResourceLimit,16> resource_limits{};
    bool operator==(const ProcessKernelState&) const = default;
    template<class Archive> void serialize(Archive& ar);
};

long emu_process_syscall(const syscall_info& info);
// Called once after the startup prelude and scratch page initialization.
// Imports supported inherited state and rejects kernel state not yet modeled.
void capture_process_kernel_state(int pid, int node);
#endif
