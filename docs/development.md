# Developing DetSim

For storage APIs, persistence boundaries and memory limits, see the
[current storage contract](state_store_full_guide.md).

For the project's actual model-checking requirements, start with the
[state and atom contract assessment](model-checking-contract.md). It includes a
reproduced SIMD restore failure and distinguishes state closure from schedule
coverage; passing the current gates does not establish either general guarantee.

## Commands

From the repository root, with GNU Make 4.3+, Python 3 and a C++20 compiler:

```sh
make help
make check
make check-storage       # storage faults/concurrency/reopen + read-only tools; NP=1
make check-integration   # production DWARF/ELF fixtures; default NP=1
make check-scenarios     # build NP=1 tracer and run isolated native ptrace scenarios
make check-futex check-futex-snapshot # NP=1 wait/wake/requeue and branch restore
make check-xstate check-exec check-state-io check-mappings # NP=1 state/atom gates
make check-identity check-port # NP=2 identity and deterministic network state
make check-accept        # NP=2 cross-process accept address/length regressions
make check-redis         # NP=2 fixed-schedule SET/GET with existing Redis debug binary
make benchmark-redisraft # NP=3 bounded Raft-library replication and DFS benchmark
make NP=1 -j4
make NP=3 debug -j4
```

`make check` runs component tests and build-contract regressions. It does not use
ptrace, start workloads, disable ASLR, or modify snapshot stores. `NP` is required
for the tracer and must match the scenario's `Nodes`; tests choose their own scope.

Compilers can be selected explicitly, including when PATH contains a custom toolchain:

```sh
make check CC=/usr/bin/gcc CXX=/usr/bin/g++
make NP=1 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4
```

Production dependencies: libdw, libdwarf, libelf, readline, zstd, xxhash, libunwind,
fmt, ncurses and the legacy cJSON headers. nlohmann/json is vendored. Cereal
headers can come from the initialized `third_party/cereal` submodule or the system
development package; a plain clone does not populate that submodule automatically.
Flex and Bison regenerate expression parser outputs when their inputs change.
No dependency download occurs as part of a build. `ccache` is optional:
`CXX="ccache g++"`.

The supported local baseline is libdwarf 0.11.1 and Debian GCC 14.2
(`/usr/bin/g++`). The removed DWARF API calls have been migrated and the tracer
now builds. A custom GCC 15.2 installation in PATH produces a pthread-header error;
select the system compiler explicitly in this environment. Older libdwarf API
versions are not part of the verified baseline.

Objects live in `build/release/npN` or `build/debug/npN`. A configuration stamp
invalidates them when the compiler command or flags change. Returning to another
NP/mode reuses its own objects and relinks `tracer`. Compiler binaries replaced at
the same path are not detected; rebuild with a fresh build directory in that case.
Do not run two independent make processes targeting the same output concurrently.

`make clean` removes ordinary build output, not tracked parser sources or runtime
snapshots. `make clear` deliberately deletes `memory/` and `sstate/`; it is not a
verification step. PGO uses `make NP=N pgo-generate`, workload execution, then
`make NP=N pgo-use` in separate invocations. Profiles and PGO objects are separated
by NP. Build tests check phase switching, not profile quality or optimized behavior.

Optional `make NP=N benchmark` and `make NP=N tools` compile performance/query
utilities; they are not part of the component correctness gate.

## Active code paths

| Responsibility | Current entry / owner |
| --- | --- |
| Startup | `src/main.cpp`, argument/config parsing in `core/config.cpp` |
| CLI exploration | `ui/cli/commands.cpp` → `exec_bfs`, `exec_dfs`, `exec_rand` in `core/scheduler.cpp` |
| State transition | `core/state/state_transition.cpp` → restore, execute, capture |
| Tracee execution | `core/engine/exec_engine.cpp`, thread and signal managers |
| Syscall interception | `core/syscall/dispatcher.cpp`, `handlers.cpp` |
| Capture/restore and identity | `core/state/state.cpp`, `state.h`, `serialize.cpp` |
| Storage | `state_store*`, `sysstate_store*` under `core/state/` |
| Emulated resources | `subsys/fs`, `subsys/net`, `subsys/sync` |
| Inspection | `core/dwarf.cpp`, `utils/expr*`, `core/syscall_fmt.cpp` |

`core/scheduler/exploration.cpp` is a second, compiled exploration implementation
with `scheduler_exec_*` entry points. Current CLI calls the older `exec_*` entry
points. Do not assume changes to the new implementation affect normal CLI runs.
Consolidation requires equivalence tests; see the backlog.

## Verification levels

- Component tests exercise real subsystem implementations using deterministic local
  inputs. They do not establish fidelity to all Linux behaviors.
- Build-contract tests run the real Makefile with a fake compiler in temporary
  directories. They verify failure propagation and configuration invalidation;
  a separate real compiler build checks C++ compilation/linking.
- Legacy local DWARF/storage test experiments are not an established suite. Several
  were ignored by Git; the DWARF stubs include an incompatible fake runtime layout.
  Do not restore those to the default gate merely by suppressing compile errors.
- `make check-integration` links the production parser against real DWARF 4/5
  multi-CU fixtures and tests ELF Go-section classification. It does not use ptrace.
- `make check-scenarios NP=1` runs real tracer scenarios: interactive/batch
  commands, filename-independent startup, finite execution, restore/replay identity and SIMD state,
  finite BFS and a reachable violation, normal/SIGTERM child cleanup and automatic-search exception cleanup. Each run
  owns a temporary directory, session and timeout. Host ASLR is not changed.
  Ptrace must be permitted by the execution environment; permission failures fail
  the gate rather than silently skip it. These are NP=1 single-thread checks.
- For an already-built NP=1 tracer: `python3 tests/tracer_scenarios.py`.
  `DETSIM_TRACER` selects another binary; `CC` selects the fixture compiler.
  Optional `--profile TracerScenarios.test_restore_and_replay_preserve_counter_and_state_identity`
  runs that replay case under installed `strace` to count `madvise` calls.
  Elapsed times include CLI/strace overhead and are not throughput benchmarks.

The scenario fixture intentionally ignores write return values: VFS currently
rejects standard-stream writes (C15). Loader CPU-feature normalization also logs
an unresolved symbol (C17). Passing counter/replay tests does not close either gap,
nor does it prove multithread, cross-process-run or eviction correctness.

Snapshot format is now version 5; old snapshots are rejected without migration.
Private readable mappings are captured; shared/unreadable mappings, overlaps with
the reserved scratch page and missing file-backed restore mappings fail explicitly.
Thread slots and futex queues survive restore for unchanged live physical thread
sets. Restoring across thread creation/exit is rejected. Timed/PI futex operations
are unsupported and fail explicitly; successful waits remain suspended until wake.

Run the NP=1 and NP=2 gates in separate make invocations. The Redis gate requires
the existing `examples/redis/redis/src/redis-server`; it downloads nothing and
does not modify the submodule. It forces C locale, compiles the example client
with assertions enabled, and saves each run under `build/redis-check/run-*`.
Default bounds are 60 seconds, 1000 steps per stage and 2 GiB address space.
Success requires both exact client RESP checks and normal virtual exit, not merely
server readiness. See [Redis evidence and model limits](redis-smoke.md).

For the three-node Raft-library workload, reproducible timings and measurement
limits are documented in [redisraft-benchmark.md](redisraft-benchmark.md).

Before finishing a change, run `git diff --check`, the relevant tests, and a fresh
build if production/build files changed. Use compiler warnings/static analysis on
the touched components; do not bury a focused change in whole-project formatting.

## Where information belongs

- `AGENTS.md`: durable contributor rules, short enough to read every session.
- This document: runnable commands, call paths, verification boundaries.
- `correctness-backlog.md`: open defects and the tests needed to close them.
- `maintenance-plan.md`: scope and evidence for the initial maintenance pass.
- Historical reports: retain for context; do not treat their success claims as gates.

Future work should update these files instead of adding another overlapping
architecture summary or an unverified “completed” report.
