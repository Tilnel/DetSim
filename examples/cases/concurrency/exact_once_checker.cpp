// Protocol oracle via public output/state views; never infer runtime deadlock.
#include "api/plugin.h"
#include <cstdio>
#include <cstring>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/syscall.h>

namespace {
struct Monitor { uint32_t output_checked = 0; uint32_t worker_observed = 0; };

detsim_verdict fail(detsim_diagnostic diagnostic, const char* message) {
    if (diagnostic.data && diagnostic.capacity)
        std::snprintf(diagnostic.data, diagnostic.capacity, "%s", message);
    return DETSIM_VIOLATION;
}
detsim_verdict save(Monitor monitor, detsim_writer writer) {
    return writer.append && !writer.append(writer.context, &monitor, sizeof monitor) ? DETSIM_PASS : DETSIM_ERROR;
}
detsim_verdict init(const detsim_host_v2*, const detsim_state_view*, const char*,
                    detsim_writer writer, detsim_diagnostic) { return save({}, writer); }
int append_json(void* context, const void* data, size_t size) {
    static_cast<std::string*>(context)->append(static_cast<const char*>(data), size);
    return 0;
}
detsim_verdict advance(const detsim_host_v2* host, const detsim_state_view*,
                       const detsim_state_view* after, const detsim_event_v2* event,
                       detsim_bytes previous, detsim_writer writer, detsim_diagnostic diagnostic) {
    if (previous.size != sizeof(Monitor) || !previous.data) return DETSIM_ERROR;
    Monitor monitor;
    std::memcpy(&monitor, previous.data, sizeof monitor);
    if (event->phase == DETSIM_EXECUTION_STOPPED)
        return fail(diagnostic, "exact-once runtime crashed before completion");
    if (event->phase == DETSIM_SYSCALL_COMPLETED && event->result_valid &&
        event->syscall_number == SYS_write && event->args[0] == 1 && event->result > 0) {
        char output[128]{};
        size_t copied = 0;
        const detsim_buffer_selector selector{DETSIM_BUFFER_FILE, 1, 0, 0, 0};
        if (!host->read_buffer || host->read_buffer(after, event->process, &selector,
                                                   output, sizeof(output)-1, &copied))
            return DETSIM_UNAVAILABLE;
        if (copied == sizeof(output)-1)
            return fail(diagnostic, "exact-once protocol exceeded bounded output");
        if (std::strchr(output, '\n')) {
            if (std::strcmp(output, "EXACT_ONCE 0 1\n"))
                return fail(diagnostic, "exact-once producer/consumer mismatch: expected task IDs 0 and 1 once each");
            std::string metadata;
            const detsim_writer sink{&metadata, append_json};
            if (!host->inspect_json || host->inspect_json(after, event->process, sink))
                return DETSIM_UNAVAILABLE;
            try {
                const auto state = nlohmann::json::parse(metadata);
                if (state.at("threads").size() < 2)
                    return fail(diagnostic, "exact-once completion did not create a runtime worker thread");
            } catch (...) { return DETSIM_ERROR; }
            monitor.worker_observed = 1;
            monitor.output_checked = 1;
        }
    }
    if (event->phase == DETSIM_THREAD_EXITED && event->syscall_number == SYS_exit_group &&
        (event->args[0] != 0 || !monitor.output_checked || !monitor.worker_observed))
        return fail(diagnostic, "exact-once runtime exited without checked concurrent completion");
    return save(monitor, writer);
}
const detsim_plugin_v2 plugin{DETSIM_PLUGIN_ABI_V2, sizeof(detsim_plugin_v2),
    "cases.runtime-exact-once", 1, nullptr, nullptr, init, advance, nullptr, nullptr};
}
extern "C" const detsim_plugin_v2* detsim_plugin_entry_v2() { return &plugin; }
