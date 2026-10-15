# Developing DetSim

For storage APIs, persistence boundaries and memory limits, see the
[current storage contract](state_store_full_guide.md).

For the project's actual model-checking requirements, start with the
[state and atom contract assessment](model-checking-contract.md). It includes a
reproduced SIMD restore failure and distinguishes state closure from schedule
coverage; passing the current gates does not establish either general guarantee.

## Development branch

`master` is the main development branch. The snapshot and generic-framework work
has been fast-forwarded into it; ordinary follow-up development now commits
directly on `master`. Historical experiment/refactor branches remain as references.

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
make check-capture       # NP=2 full/hinted snapshot byte-equivalence oracle
make check-soft-dirty    # opt-in experiment: real kernel dirty tracking + byte oracle
make check-page-snapshot # flat/page readers, missing/corrupt pages, reopen and poison gates
make check-syscall-policy check-process-state # NP=2 closed syscall/model gates
make check-state-view check-plugins check-runtime-profile # NP=2 generic framework gates
make check-io-boundary   # NP=2 mixed descriptors, guest-copy faults and native dispatch
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

Standard streams use captured virtual files: stdin is EOF, stdout/stderr writes
enter snapshot state. Loader CPU-feature normalization still logs an unresolved
symbol (C17). Passing counter/replay tests does not close that gap,
nor does it prove multithread, cross-process-run or eviction correctness.

Default snapshot format is version 11; older formats are rejected without migration.
This branch additionally reads opt-in version 12 page manifests, selected with
`DETSIM_SNAPSHOT_BACKEND=pages`; `verify-pages` adds a full memory oracle.
See [page snapshots and whole-exploration measurements](page-snapshots.md).
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
The benchmark's `--perf PATH --perf-sudo` options record DFS-only CPU call stacks;
`tests/redisraft_profile_summary.py` groups exported stacks into disjoint phases.
CPU sampling proportions and full-command wall time answer different questions.
The soft-dirty experiment is disabled by default. See the measured costs and
explicit support limits in [the Raft benchmark](redisraft-benchmark.md).

Before finishing a change, run `git diff --check`, the relevant tests, and a fresh
build if production/build files changed. Use compiler warnings/static analysis on
the touched components; do not bury a focused change in whole-project formatting.

## Framework and plugin boundary

`SnapshotView::open(hash)` is the one saved-memory interface for flat and page
snapshots. Validation, address traversal and owner lifetime are shared; internal
`FlatMemory` and `PagedMemory` implementations supply immutable chunks. Consumers
use `read_into` or owned spans, without checking the encoding. `StateView` groups
these readers by logical process for inspection, independently of the UI cursor.
It also exposes saved clocks and a read-only expression evaluator. `inspect_json`
returns schema1 logical-thread/register/resource metadata (no physical IDs or bulk
payloads); `read_buffer` reads file, pipe, TCP, UDP and pre-accept buffers without
consuming data or moving offsets. Pending endpoint selectors are lossless decimal
strings. Named lifecycle fields avoid importing private status headers. Metadata
is decoded once per view, with lifecycle/poison checks retained on cached queries.
Stopped-live register/futex inspection is explicitly unavailable. Missing bytes
fail; saved readers never fall back to live memory. The explicit `stopped_live()`
view is reserved for environment callbacks before capture.

Public plugins include only `src/api/plugin.h` and export
`detsim_plugin_entry_v1`. Build the `.so` explicitly. Loading legacy `check()` or
compiling sources while parsing configuration is unsupported. An example:

```json
{
  "Plugins": [{"Path": "./checker.so", "Configuration": {"limit": 4}}],
  "RuntimeProfile": {
    "startup_syscalls": [10, 11],
    "deterministic_at_random": true,
    "disable_vdso": true,
    "ignored_signals": []
  }
}
```

Callbacks receive a borrowed host table and opaque read-only views. Configuration
is available in each checker/environment module's host table. No STL ownership or physical PID crosses
the C ABI. Plugins are trusted native code: the interface is a contract, not a
sandbox. Callback results distinguish pass, violation, unavailable and error;
unavailable data does not count as a passing check.

- `check_state` inspects one snapshot; `check_transition` inspects source, event
  and target. These cannot modify guest memory or persistent plugin globals.
- Optional `init_monitor`/`advance_monitor` produce canonical versioned history
  bytes through a host writer. Monitor bytes are copied/archived per search branch
  and included in system identity, separately from machine snapshots.
- `choose_time` reads a stopped-live view and returns a validated time and budget
  cost. Guest copyout succeeds before clock/budget commit. V1 does not expose
  arbitrary syscall-output mutation or claim a universal environment-plugin API.
- Optional byte decoders only affect presentation. A decoder-only plugin does not
  enter model identity or monitor state.

Every completed transition captures a candidate machine, checks predicates and
advances monitors before committing metadata, publishing history or deduplicating.
BFS, DFS, random and manual execution use this boundary. A violation retains a
replayable terminal product state with a persisted failure bit; terminal nodes
can be inspected but cannot execute another transition. Monitor callbacks returning
violation discard partial output and preserve previous history; inspection errors
abort without publishing a
successor. Pure predecessor/target comparisons need no monitor state.

Plugin binaries/schema/configuration, target executable contents and semantic
execution configuration form the model compatibility fingerprint. Executables must remain immutable during a run. Changing cache
sizes or loading a decoder does not change it. Flat11/page12 and the versioned
system metadata reject older archives without deleting or migrating stores.
Changes to kernel-model or inspection semantics require an explicit compatibility
version bump; fingerprints do not prove equivalence between different implementations.

Symbolic observation currently supports the matching non-PIE main executable.
Mismatched binaries, PIE/shared-image relocation or unavailable types fail
explicitly. Raw address reads are independent of symbol availability. Expression
assignment and increment/decrement are rejected, including dead branches.

The default bootstrap profile explicitly stops after Linux dynamic-loader
`munmap`; static libc fixtures use `mprotect`, and heterogeneous targets can supply
one boundary per process. The prelude executes outside exploration and is not a
proof that arbitrary runtime initialization is deterministic. Its syscall count
limit cannot interrupt a blocking syscall. Go guessing, unconditional SIGURG
suppression and the glibc-private CPU-offset patch were removed. Signal delivery
requires additional saved state; currently fatal signals stop execution, explicitly
configured ignores suppress, and other signals are unsupported.

## Current support boundary

Generic plugin/state boundaries are implemented; this is not yet a general Linux
process virtualizer or a complete shared-memory model checker. Startup, controlled
execution, faithful restore and exhaustive exploration are separate support levels.

| Area | Main remaining gap | Consequence |
| --- | --- | --- |
| Kernel-state closure (P0) | Unclassified syscalls reject; signal/timer delivery and CPU nondeterminism are not fully modeled | Explicit syscall support is scoped; asynchronous and CPU inputs still need a complete contract |
| Scheduling/atoms (P0) | Runnable thread/outcome enumeration, all-thread stop barrier, bounded pure computation and shared-memory access boundaries | Syscall stepping does not cover all concurrent behavior; weak-memory semantics also require an explicit model |
| Lifecycle/restore (P0) | Thread creation/exit reconstruction and process trees/fork/exec | Changed physical thread sets are rejected; parked virtual-exit/fatal images now restore across branches (C02/C08) |
| Blocking/event loop (P1) | Wait registration, readiness wakeups, deadlines, timeout ordering and signal interruption | Returning EAGAIN/zero is not Linux blocking semantics; sleep and epoll timing are incomplete |
| Files/IPC/network (P1) | Reusable rootfs and external inputs, shared filesystem/open-file descriptions, dup/vector/positioned I/O, directory mutations, Unix/IPv6 sockets, eventfd/timerfd/io_uring | Many common runtime interfaces explicitly reject; fsync does not model crash durability |
| Mappings (P1) | Shared/unreadable mappings, guard/reserved pages and mutable file-backed coherence | Common allocators/JITs/shared-memory applications can hit explicit unsupported cases |
| Startup/inspection (P1) | Portable initial boundary, per-node PIE/shared-image relocation and symbol contexts | PIE execution is possible; symbolic property inspection is more restricted than execution. Debug symbols are optional for raw/resource inspection |
| Scale/search (P2) | Runtime process count, state equivalence/canonicalization, partial-order reduction and liveness/fairness algorithms | Fixed NP and bounded safety searches are not arbitrary process-graph or liveness support |

Next work should first close the unsoundness paths: classify/reject uncontrolled
host effects, extend lifecycle restoration beyond parked terminal images, then establish
stop-all + explicit actor/outcome selection. Expand syscall coverage on top of
those contracts, with restore/replay and successor-set tests. Keep shared-memory
instruction boundaries as a separate substantial backend effort; adding syscall
names alone cannot supply them. Detailed evidence and acceptance tests remain in
[the correctness backlog](correctness-backlog.md).

## Where information belongs

- `AGENTS.md`: durable contributor rules, short enough to read every session.
- This document: runnable commands, call paths, verification boundaries.
- `correctness-backlog.md`: open defects and the tests needed to close them.
- `maintenance-plan.md`: scope and evidence for the initial maintenance pass.
- Historical reports: retain for context; do not treat their success claims as gates.

Future work should update these files instead of adding another overlapping
architecture summary or an unverified “completed” report.

## Closed syscall profile

Exploration rejects unregistered system calls before kernel execution. The native
allowlist is limited to owned-process identity/credential queries, query-only
resource limits and supported private memory operations. Thread creation and
thread exit remain unsupported; `exit_group` retains the virtual termination path.
The trusted ELF prelude and internal restore injections are separate boundaries.

Clocks use the saved virtual clock. `getrandom` uses a saved deterministic stream;
this does not enumerate random outcomes. System memory reports a fixed model
profile. Signal dispositions and filesystem umask are branch state; custom signal
delivery and armed interval timers are unsupported. These choices improve state
closure within an explicit environment, not compatibility with all Linux programs.
