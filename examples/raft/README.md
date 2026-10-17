# Willemt Raft library

`raft/` is the vendored Willemt Raft library tree tracked by the main repository,
not a Git submodule. `raft_fixture.c` drives a three-node
cluster; `run.py` builds owned copies for healthy and deliberately mutated native
and model checks without modifying the vendored library. `raft_time_choice.cpp` is the
case clock-choice plugin. Existing `plugins/` and JSON configurations remain this case's legacy
driver/plugin inputs.

```sh
python3 examples/raft/run.py --tracer ./tracer --backend all
python3 examples/raft/run.py --native-only
```

The controlled runner requires NP=3. `run.py` defines the three tracees,
addresses, startup profile, choice configuration and bounded search/replay policy;
the exact generated configuration is `<session>/config.json`, with reports and
hashes beneath `--output` (default `build/raft-cases`). Native-only success is not
model-check success; bounded results do not establish exhaustive interleavings.
