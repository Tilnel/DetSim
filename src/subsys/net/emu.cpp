#include "emu.h"
#include "guest.h"
#include "monitor.h"
#include "plugins.h"
#include "state/state_view.h"
#include <cerrno>
#include <stdexcept>

int choose_many[450] = {};
int emu_gettimeofday(struct timeval *tv, struct timezone *tz)
{
  int node = ptmc_state.cursor;
  timeval result = ptmc_state.time[node];
  unsigned cost = 0;
  if (ptmc_state.n_choose != 0)
  {
    if (ptmc_state.error_bound < 0) throw std::runtime_error("negative model budget");
    auto choice = detsim::plugins::choose_time(detsim::StateView::stopped_live(), node,
      ptmc_state.choose, result, static_cast<unsigned>(ptmc_state.error_bound));
    result = {static_cast<time_t>(choice.value.seconds), choice.value.microseconds};
    cost = choice.budget_cost;
  }
  /* Copy first: a rejected guest pointer must not consume model budget/time. */
  if (tv && !guest_write_exact(ptmc_state.pids[node], tv, &result, sizeof(result))) return -EFAULT;
  const struct timezone zero{};
  if (tz && !guest_write_exact(ptmc_state.pids[node], tz, &zero, sizeof(zero))) return -EFAULT;
  ptmc_state.time[node] = result;
  ptmc_state.error_bound -= static_cast<int>(cost);
  return 0;
}
