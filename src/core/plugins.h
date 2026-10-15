#ifndef DETSIM_CORE_PLUGINS_H
#define DETSIM_CORE_PLUGINS_H
#include "api/plugin.h"
#include <string>
#include <sys/time.h>
struct sys_state;
namespace detsim { class StateView; namespace plugins {
void load(const std::string &path, const std::string &configuration = "{}");
void reset();
bool has_time_choice();
bool check_initial(sys_state &state);
bool check_transition(const sys_state &before, sys_state &after,
                      const detsim_event_v1 &event);
detsim_time_choice_v1 choose_time(const StateView &view, unsigned node,
                                 int choice, timeval now, unsigned budget);
std::string describe_bytes(const void *data, size_t length);
/* Fingerprint is persisted even for stateless semantic plugins: incompatible checkers,
 * model policies or configuration cannot silently reuse a search state. */
std::string context_key();
} }
#endif
