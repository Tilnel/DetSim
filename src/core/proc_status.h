#ifndef PROC_STATUS_H
#define PROC_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Process status word layout (32-bit int):
 *
 *  bits 0-7:   flags (DIFEXITED, DIFSTOPPED, etc.)
 *  bits 8-15:  exit code or stop signal (0-255)
 *  bits 16-31: reserved (must be 0)
 *
 *  Special value:
 *    0x00000000 = normal (running, not exited, not stopped)
 */

/* Status flags in bits 0-7 */
#define DIFEXITED    0x01   /* Process exited normally */
#define DIFSTOPPED   0x02   /* Process stopped by signal */
#define DIFSIGNALED  0x04   /* Process terminated by signal (uncaught)
*/
#define DIFCRASHED   0x08   /* Special: crashed (SIGSEGV/SIGABRT) */

/* Extract exit status (valid if DIFEXITED) */
#define DEXITSTATUS(status)   (((status) >> 8) & 0xff)

/* Extract stop signal (valid if DIFSTOPPED) */
#define DSTOPSIG(status)      (((status) >> 8) & 0xff)

/* Extract termination signal (valid if DIFSIGNALED) */
#define DTERMSIG(status)      (((status) >> 8) & 0xff)

/* Check if status indicates process is dead (exited, signaled, or cra
shed) */
#define DISDEAD(status)       ((status) & (DIFEXITED | DIFSIGNALED | DIFCRASHED))
#define DISCRASHED(status)    ((status) & DIFCRASHED)
#define DISSIGNALED(status)   ((status) & DIFSIGNALED)
#define DISEXITED(status)     ((status) & DIFEXITED)

/* Check if status indicates process is alive (running or stopped) */
#define DISALIVE(status)      (!DISDEAD(status))

/* Convenience: check specific crash types */
#define DISSEGV(status)       (((status) & DIFCRASHED) && DSTOPSIG(status) == 11)
#define DISABRT(status)       (((status) & DIFCRASHED) && DSTOPSIG(status) == 6)

/* Constructor functions */

/* Normal running status */
static inline int dstatus_normal(void) {
    return 0;
}

/* Exited with given exit code (0-255) */
static inline int dstatus_exit(int exitcode) {
    return ((exitcode & 0xff) << 8) | DIFEXITED;
}

/* Stopped by signal (e.g., SIGSTOP, SIGTSTP) */
static inline int dstatus_stop(int sig) {
    return ((sig & 0xff) << 8) | DIFSTOPPED;
}

/* Crashed by signal (SIGSEGV, SIGABRT, etc.)
 * Similar to stopped but marked as crashed */
static inline int dstatus_crash(int sig) {
    return ((sig & 0xff) << 8) | DIFCRASHED;
}

/* Terminated by uncaught signal (not crash, not exit) */
static inline int dstatus_signal(int sig) {
    return ((sig & 0xff) << 8) | DIFSIGNALED;
}

#ifdef __cplusplus
}
#endif

#endif /* PROC_STATUS_H */
