#ifndef DETSIM_API_PLUGIN_H
#define DETSIM_API_PLUGIN_H

/* Explicit manual timeout transition; ordinary transitions retain -1/choices. */
enum { DETSIM_CHOICE_TIMEOUT = -2 };
/* Stable C ABI. Plugins may inspect supplied views, never the live runtime.
 * Every callback is synchronous. Views, host, diagnostics and writers are borrowed
 * for the callback only. Plugins must not retain pointers or mutate globals as
 * semantic state; history belongs in monitor bytes. No exceptions cross the ABI. */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define DETSIM_PLUGIN_ABI_V2 2u
struct detsim_state_view;
typedef enum detsim_verdict {
  DETSIM_PASS = 0, DETSIM_VIOLATION = 1,
  DETSIM_UNAVAILABLE = 2, DETSIM_ERROR = 3
} detsim_verdict;
typedef struct detsim_diagnostic { char *data; size_t capacity; } detsim_diagnostic;
typedef struct detsim_bytes { const uint8_t *data; size_t size; } detsim_bytes;
/* Monitor output is committed only on PASS. On a monitor callback VIOLATION the initial
 * node has empty monitor bytes, or a transition keeps its previous bytes;
 * partial output is always discarded. UNAVAILABLE/ERROR abort the transition.
 * Violating nodes are terminal and cannot be used as a transition source. */
typedef struct detsim_writer {
  void *context;
  int (*append)(void *context, const void *bytes, size_t length);
} detsim_writer;
typedef struct detsim_time { int64_t seconds; int32_t microseconds; } detsim_time;
typedef enum detsim_event_phase {
  DETSIM_SYSCALL_BLOCKED = 1, DETSIM_SYSCALL_COMPLETED = 2,
  DETSIM_EXECUTION_STOPPED = 3, DETSIM_THREAD_EXITED = 4
} detsim_event_phase;
typedef struct detsim_event_v2 {
  uint32_t process;
  uint32_t thread;
  int32_t syscall_number;
  int32_t choice;
  uint64_t args[6];
  int64_t result; // Valid only when result_valid != 0; zero otherwise.
  uint32_t phase; // detsim_event_phase; only COMPLETED is a syscall return.
  uint32_t result_valid;
} detsim_event_v2;
/* Read-only payload selectors. item is an endpoint key for TCP_PENDING and zero
 * otherwise; message indexes a TCP chunk/UDP datagram, zero for FILE/PIPE.
 * offset may equal the selected buffer size (EOF), but may not exceed it. */
typedef enum detsim_buffer_kind {
  DETSIM_BUFFER_FILE = 1, DETSIM_BUFFER_PIPE = 2, DETSIM_BUFFER_TCP = 3,
  DETSIM_BUFFER_UDP = 4, DETSIM_BUFFER_TCP_PENDING = 5
} detsim_buffer_kind;
typedef struct detsim_buffer_selector {
  uint32_t kind;
  int32_t fd;
  uint64_t item, message, offset;
} detsim_buffer_selector;
typedef struct detsim_host_v2 {
  uint32_t abi_version;
  size_t (*process_count)(const struct detsim_state_view *);
  int (*read)(const struct detsim_state_view *, uint32_t process,
              uint64_t address, void *destination, size_t length);
  int (*evaluate_i64)(const struct detsim_state_view *, uint32_t process,
                      const char *expression, int64_t *value);
  int (*time)(const struct detsim_state_view *, uint32_t process, detsim_time *);
  /* Canonical JSON, borrowed for this callback. No global plugin configuration. */
  const char *configuration;
  /* Canonical resource metadata schema 1, without bulk payloads/physical IDs.
   * Live views explicitly mark register/futex metadata unavailable. */
  int (*inspect_json)(const struct detsim_state_view *, uint32_t process, detsim_writer);
  /* Returns 0 on success, -1 on unavailable/invalid request; *copied may be
   * shorter than length at EOF. No descriptor offset or queue is consumed. */
  int (*read_buffer)(const struct detsim_state_view *, uint32_t process,
                     const detsim_buffer_selector *, void *destination, size_t length, size_t *copied);
} detsim_host_v2;
typedef struct detsim_time_choice_v2 {
  detsim_time value;
  uint32_t budget_cost;
} detsim_time_choice_v2;
typedef struct detsim_plugin_v2 {
  uint32_t abi_version;
  uint32_t struct_size;
  const char *id;
  uint32_t monitor_schema;
  detsim_verdict (*check_state)(const detsim_host_v2 *, const struct detsim_state_view *, detsim_diagnostic);
  detsim_verdict (*check_transition)(const detsim_host_v2 *, const struct detsim_state_view *, const struct detsim_state_view *, const detsim_event_v2 *, detsim_diagnostic);
  detsim_verdict (*init_monitor)(const detsim_host_v2 *, const struct detsim_state_view *, const char *configuration, detsim_writer, detsim_diagnostic);
  detsim_verdict (*advance_monitor)(const detsim_host_v2 *, const struct detsim_state_view *, const struct detsim_state_view *, const detsim_event_v2 *, detsim_bytes previous, detsim_writer next, detsim_diagnostic);
  detsim_verdict (*choose_time)(const detsim_host_v2 *, const struct detsim_state_view *, uint32_t process, int32_t choice, detsim_time now, uint32_t budget, detsim_time_choice_v2 *, detsim_diagnostic);
  /* Optional presentation only: 0 = unrecognized, 1 = decoded, -1 = error. */
  int (*describe_bytes)(detsim_bytes, detsim_writer);
} detsim_plugin_v2;
typedef const detsim_plugin_v2 *(*detsim_plugin_entry_v2_fn)(void);
/* Each shared object exports exactly this entry point. */
const detsim_plugin_v2 *detsim_plugin_entry_v2(void);
#ifdef __cplusplus
}
#endif
#endif
