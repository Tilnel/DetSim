/* Case-specific oracle and finite time inputs, using only the public plugin ABI.
 * History is branch-owned monitor data, never shared-library globals. */
#include "api/plugin.h"
#include <cstdio>
#include <cstring>
#include <limits>
#include <sys/syscall.h>

namespace {
struct Monitor {
    uint32_t magic = 0x43415345;
    uint32_t choices = 0;
    uint32_t bits = 0;
    uint32_t output_seen = 0;
    uint32_t terminal = 0;
};
static_assert(sizeof(Monitor) == 20);

detsim_verdict error(detsim_diagnostic d, const char *message) {
    if (d.data && d.capacity) std::snprintf(d.data, d.capacity, "%s", message);
    return DETSIM_ERROR;
}
detsim_verdict save(const Monitor &m, detsim_writer writer) {
    return writer.append && !writer.append(writer.context, &m, sizeof m) ? DETSIM_PASS : DETSIM_ERROR;
}
detsim_verdict init(const detsim_host_v2 *, const detsim_state_view *, const char *,
                    detsim_writer writer, detsim_diagnostic) { return save(Monitor{}, writer); }

detsim_verdict advance(const detsim_host_v2 *host, const detsim_state_view *,
                       const detsim_state_view *after, const detsim_event_v2 *event,
                       detsim_bytes previous, detsim_writer writer, detsim_diagnostic diagnostic) {
    if (previous.size != sizeof(Monitor) || !previous.data) return error(diagnostic, "invalid case monitor");
    Monitor m;
    std::memcpy(&m, previous.data, sizeof m);
    if (m.magic != Monitor{}.magic || event->process != 0) return error(diagnostic, "unexpected case process or schema");
    const bool completed = event->phase == DETSIM_SYSCALL_COMPLETED && event->result_valid;
    if (completed && event->syscall_number == SYS_gettimeofday) {
        if (event->result != 0 || event->choice < 0 || event->choice > 1 || m.choices >= 2)
            return error(diagnostic, "invalid case time input");
        m.bits = (m.bits << 1) | static_cast<uint32_t>(event->choice);
        ++m.choices;
    }
    if (completed && event->syscall_number == SYS_write && event->args[0] == 1) {
        if (m.output_seen || m.choices != 2 || event->result <= 0 || event->result >= 64)
            return error(diagnostic, "missing, duplicate or partial case output");
        char output[64]{};
        size_t copied = 0;
        const detsim_buffer_selector selector{DETSIM_BUFFER_FILE, 1, 0, 0, 0};
        if (!host->read_buffer || host->read_buffer(after, 0, &selector, output, sizeof(output)-1, &copied))
            return error(diagnostic, "case output unavailable");
        if (copied != static_cast<size_t>(event->result)) return error(diagnostic, "case output length mismatch");
        char expected[64];
        const int a = (m.bits >> 1) & 1, b = m.bits & 1;
        std::snprintf(expected, sizeof expected, "CASE_OK %d%d %d\n", a, b, a || b ? 0 : 1);
        if (std::strcmp(output, expected) == 0) m.output_seen = 1;
        else if (m.bits == 3 && std::strcmp(output, "CASE_BUG 11 -1\n") == 0) {
            if (diagnostic.data && diagnostic.capacity)
                std::snprintf(diagnostic.data, diagnostic.capacity,
                              "account overdraft: inputs=11 balance=-1 (expected >= 0)");
            return DETSIM_VIOLATION;
        } else return error(diagnostic, "case output disagrees with independent oracle");
    }
    if (event->phase == DETSIM_THREAD_EXITED) {
        if ((event->syscall_number != SYS_exit && event->syscall_number != SYS_exit_group) ||
            event->args[0] != 0 || m.choices != 2 || !m.output_seen)
            return error(diagnostic, "case exited without a checked successful result");
        // Descriptors are already closed at exit. Keep the previously checked result.
        m.terminal = 1;
    }
    return save(m, writer);
}

detsim_verdict choose(const detsim_host_v2 *, const detsim_state_view *, uint32_t process,
                      int32_t choice, detsim_time now, uint32_t, detsim_time_choice_v2 *out,
                      detsim_diagnostic diagnostic) {
    if (process != 0 || choice < 0 || choice > 1) return error(diagnostic, "case expects two time outcomes");
    out->value = now;
    out->budget_cost = 0;
    if (choice && ++out->value.microseconds == 1000000) {
        if (now.seconds == std::numeric_limits<int64_t>::max()) return error(diagnostic, "case clock overflow");
        ++out->value.seconds; out->value.microseconds = 0;
    }
    return DETSIM_PASS;
}
int describe(detsim_bytes bytes, detsim_writer writer) {
    if (bytes.size != sizeof(Monitor) || !bytes.data) return 0;
    Monitor m;
    std::memcpy(&m, bytes.data, sizeof m);
    if (m.magic != Monitor{}.magic) return 0;
    char text[128];
    const int size = std::snprintf(text, sizeof text,
        "{\"choices\":%u,\"bits\":%u,\"output_seen\":%u,\"terminal\":%u}",
        m.choices, m.bits, m.output_seen, m.terminal);
    return writer.append && !writer.append(writer.context, text, static_cast<size_t>(size)) ? 1 : -1;
}
const detsim_plugin_v2 plugin{DETSIM_PLUGIN_ABI_V2, sizeof(detsim_plugin_v2),
    "cases.binary-input-account", 1, nullptr, nullptr, init, advance, choose, describe};
}
extern "C" const detsim_plugin_v2 *detsim_plugin_entry_v2(void) { return &plugin; }
