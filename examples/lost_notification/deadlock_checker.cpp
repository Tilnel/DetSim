/* Closed-world case oracle, not a general liveness proof. The fixture has no
 * external actors, signal delivery, or timers. Public snapshot metadata suffices
 * to identify all remaining live tasks waiting for a notification that no task
 * can supply. No knowledge of pthread memory layouts is used. */
#include "api/plugin.h"
#include "nlohmann/json.hpp"
#include <cstdio>
#include <string>

namespace {
int append(void *context, const void *data, size_t size) {
    try {
        static_cast<std::string*>(context)->append(static_cast<const char*>(data), size);
        return 0;
    } catch (...) { return -1; }
}
detsim_verdict check(const detsim_host_v2 *host, const detsim_state_view *,
                      const detsim_state_view *after, const detsim_event_v2 *,
                      detsim_diagnostic diagnostic) {
    try {
        if (!host->process_count || host->process_count(after) != 1 || !host->inspect_json)
            return DETSIM_UNAVAILABLE;
        std::string text;
        if (host->inspect_json(after, 0, {&text, append})) return DETSIM_UNAVAILABLE;
        const auto state = nlohmann::json::parse(text);
        if (state.at("schema") != 1) return DETSIM_UNAVAILABLE;
        const auto &threads = state.at("threads");
        if (!threads.is_array()) return DETSIM_UNAVAILABLE;
        size_t alive = 0;
        for (const auto &thread : threads) {
            if (!thread.at("alive").get<bool>()) continue;
            ++alive;
            const auto &call = thread.at("pending_syscall");
            if (call.is_null() || call.at("phase") != "waiting" ||
                call.at("reason") != "futex" || !call.at("deadline").is_null()) return DETSIM_PASS;
        }
        if (!alive) return DETSIM_PASS;
        if (diagnostic.data && diagnostic.capacity)
            std::snprintf(diagnostic.data, diagnostic.capacity,
                          "closed-world lost notification: all %zu live threads wait on futex without deadlines", alive);
        return DETSIM_VIOLATION;
    } catch (...) { return DETSIM_ERROR; }
}
const detsim_plugin_v2 plugin{DETSIM_PLUGIN_ABI_V2, sizeof(detsim_plugin_v2),
    "cases.closed-world-notification", 0, nullptr, check, nullptr, nullptr, nullptr, nullptr};
}
extern "C" const detsim_plugin_v2 *detsim_plugin_entry_v2(void) { return &plugin; }
