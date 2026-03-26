#include "emu.h"
#include "guest.h"
#include "monitor.h"
#include <sys/syscall.h>

/* From scheduler.cpp */
extern void apply_choose(const syscall_info &info, choose_out *out);

choose_func choose_syswhat[450] = {};
int choose_many[450] = {};

int emu_gettimeofday(struct timeval *tv, struct timezone *tz)
{
  /* Ignore tz */
  int index = ptmc_state.cursor;
  // ptmc_state.time[index].tv_sec += 1; /* Simulate time passing */
  struct timeval tracee_tv = ptmc_state.time[index];

  if (ptmc_state.n_choose == 0)
  {
    // LOG_INFO("gettimeofday: returning simulated time %ld.%06ld", tracee_tv.tv_sec,
    //          tracee_tv.tv_usec);
    memcpy_host2guest(tv, &tracee_tv, sizeof(struct timeval));
    return 0;
  }

  choose_in in(ptmc_state.time[index]);
  choose_func choose = choose_syswhat[SYS_gettimeofday];
  choose_out *out = choose(ptmc_state.pids[index], ptmc_state.choose, in);

  syscall_info info;
  info.args[0] = (uintptr_t)tv;
  info.args[1] = (uintptr_t)tz;
  apply_choose(info, out);

  ptmc_state.time[index] = *(struct timeval *)out->args[0];
  int rval = out->rval;
  delete out;
  return rval;
}
