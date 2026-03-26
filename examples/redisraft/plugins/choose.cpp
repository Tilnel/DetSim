/*
 * Raft Timeout Chooser
 *
 * Selects timeout values based on node role (Leader vs Follower)
 * Leader: uses request_timeout
 * Follower: uses election_timeout_rand
 */

#include "../raft/include/raft.h"
#include "../raft/include/raft_private.h"
#include "monitor.h"
#include "emu.h"
#include "guest.h"

static struct timeval tv_addmsec(struct timeval tv, int msec)
{
  long sec = tv.tv_sec;
  long usec = tv.tv_usec;
  usec += msec * 1000;
  sec += usec / 1000000;
  usec %= 1000000;
  return (struct timeval){sec, usec};
}

extern "C"
{
  choose_out *choose_gettimeofday(int pid, int choice, const choose_in &in)
  {
    long praft = tracee_read_word(pid, (void *)get_var_addr("raft"));
    struct raft_server raft;
    tracee_read_mem(pid, (void *)praft, &raft, sizeof(raft));

    struct timeval tv = in.now;
    struct timeval *tv_out = (struct timeval *)malloc(sizeof(*tv_out));

    // RAFT_STATE_LEADER = 4 in redisraft (was 3 in old willemt/raft)
    if (raft.state == 4)
    {
      if (choice == 0)
        *tv_out = tv;
      else
        *tv_out = tv_addmsec(tv, raft.request_timeout + 1);
    }
    else
    {
      if (choice == 0)
        *tv_out = tv;
      else
      {
        *tv_out = tv_addmsec(tv, raft.election_timeout_rand + 1);
        ptmc_state.error_bound--;
      }
    }

    choose_out *out = new choose_out();
    out->args[0] = tv_out;
    out->len[0] = sizeof(struct timeval);
    out->rval = 0;
    return out;
  }
}
