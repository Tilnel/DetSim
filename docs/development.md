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
make check-thread-control check-thread-identity check-thread-carrier # NP=1 thread gates
make check-thread-tid # NP=1 clone/clone3 TID, clear-tid/join and restore
make check-thread-lifecycle # NP=1 raw clone/exit, TLS, full/pages + cache pressure
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

Default snapshot format is version 17; older formats are rejected without migration.
This branch additionally reads opt-in version 18 page manifests, selected with
`DETSIM_SNAPSHOT_BACKEND=pages`; `verify-pages` adds a full memory oracle.
See [page snapshots and whole-exploration measurements](page-snapshots.md).
Private readable mappings are captured; shared/unreadable mappings, overlaps with
the reserved scratch page and missing file-backed restore mappings fail explicitly.
Logical thread slots and futex queues survive restore. Raw-thread births/exits
restore onto retained stopped carriers; unexpected host thread death or creation rejects. Timed/PI futex operations
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
sizes or loading a decoder does not change it. Flat17/page18 and the versioned
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
| Lifecycle/restore (P0) | Full pthread/kernel thread-state reconstruction and process trees/fork/exec | Raw births/exits rebind retained carriers; untracked physical changes reject; parked terminal images restore across branches (C02/C08) |
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
allowlist is limited to credential queries, query-only
resource limits and supported private memory operations. Thread clone/clone3/exit use the carrier model below; fork and unsupported
thread flags reject. `exit_group` retains virtual process termination.
The trusted ELF prelude and internal restore injections are separate boundaries.

Clocks use the saved virtual clock. `getrandom` uses a saved deterministic stream;
this does not enumerate random outcomes. System memory reports a fixed model
profile. Signal dispositions and filesystem umask are branch state; custom signal
delivery and armed interval timers are unsupported. These choices improve state
closure within an explicit environment, not compatibility with all Linux programs.

## Logical thread archives

Flat17/page18 archive explicit thread liveness and logical parent IDs; physical
TIDs are transient bindings. Same-run captured hashes retain compact runtime
provenance tied to an owned process pidfd, which is required on the host kernel.
Disk reload may inspect any valid snapshot, but execution restore requires local
provenance for that hash and the same live process incarnation. Bootstrap/cleanup
clear these records; state-store cache eviction does not. Host IDs may still be
present in initial guest registers/TLS/memory despite modeled getpid/gettid, so
this does not establish cross-run canonical identity or complete TID virtualization.

All nodes pass the owned stopped-thread barrier before capture injections or
restore mutations. The gate checks a preexisting stop; it does not interrupt a
running pure-user loop. Native clone-event tests prove running-worker rejection
and successful validation after stopping it. Raw carrier reconciliation now covers the explicit profile below.

## Raw thread lifecycle profile

`clone` and `clone3` require VM/FS/FILES/SIGHAND/THREAD sharing, with optional
SYSVSEM, SETTLS, PARENT_SETTID, CHILD_SETTID and CHILD_CLEARTID. Other flags reject.
Initial clear-tid registration transfers into the logical task; carrier kernel
registrations stay zero. Active kernel rseq registration still rejects birth. Nonempty/pending robust futex ownership rejects exit;
raw thread exit performs modeled clear-tid notification. The rseq/robust limits
still apply; ordinary glibc/pthread startup is not certified.
Host support for PR_GET_TID_ADDRESS and PTRACE_GET_RSEQ_CONFIGURATION is required.

The creator suppresses the guest clone, creates a kernel carrier only when the
pool has no free one, and parks it at an internal breakpoint before assigning
logical registers/TLS. Virtual exit freezes the logical slot and parks its carrier.
The physical leader remains a control anchor after logical main exits. Spare
carriers participate in the stop barrier but not snapshot identity. Slots are not
reused; the current limit is64 retained births per process, including main.

Guest getpid/gettid and clone results share a deterministic ID namespace (64 IDs
per node, main equal to process ID). Owned prlimit queries translate that namespace
without leaking a physical ID into guest registers. This does not patch IDs already
cached by arbitrary runtime initialization. `state_transition(source,node,thread)`
provides explicit runnable actor selection; the omitted thread uses existing
selection. Search does not yet enumerate every thread/outcome or shared-memory
instruction interleaving.

Every guest resume marks its physical image dirty until publication. Recovery
therefore cannot mistake a failed step for an unchanged source. Syscall-entry
rollback first cancels the pending call without executing guest code; capture
rejects entry stops. Internal carrier/injection failures abort exploration.
Process exit still lacks complete descriptor/clear-tid teardown and crash semantics.

## Runtime-independent task contract audit (2026-10-15)

The implementation target is Linux x86-64 task semantics, not pthread functions.
glibc/musl pthreads, native Go runtime threads and other runtimes should consume
the same platform model. Their user-space thread descriptors, goroutine queues,
TLS data and suspended stacks remain ordinary guest memory. Do not add runtime
structure offsets or special checkpoint extractors to the core.

A local native audit used glibc2.41 and the installed Go command, which reports
`go1.18 gccgo ...14.2.0`, not the standard gc compiler. A pthread TLS/condition/
join/robust-owner-death probe completed normally and traced clone3 with
PARENT_SETTID+CHILD_CLEARTID, set_tid_address, set_robust_list, rseq, signal masks,
futex and PROT_NONE stack reservation. Its join used shared WAIT_BITSET with
CLOCK_REALTIME and a null timeout; the present futex model already accepts that
combination, so clear-tid notification is the missing part, not this null timeout.
The gccgo goroutine/channel/sleep probe also completed normally and traced the
pthread creation path, altstack and epoll. These are native dependency inventories,
not DetSim pass results. Sources, native traces and summary are retained locally
under ignored `build/runtime-thread-audit/`; syscall counts depend on this workload
and runtime version. No dependency was installed.

Upstream gc Go separately implements OS-thread creation through clone and TLS,
masking signals around birth; its goroutine scheduler maps Gs to Ms and Ps. It
also uses signal-based asynchronous preemption. Therefore gccgo observations must
not stand in for gc Go or cgo coverage.

Sources: [glibc2.41 create](https://raw.githubusercontent.com/bminor/glibc/release/2.41/master/nptl/pthread_create.c),
[glibc2.41 join](https://raw.githubusercontent.com/bminor/glibc/release/2.41/master/nptl/pthread_join_common.c),
[Go scheduler](https://go.dev/src/runtime/HACKING),
[Go Linux threads](https://go.dev/src/runtime/os_linux.go),
[Go amd64 clone/TLS](https://go.dev/src/runtime/sys_linux_amd64.s),
[Go signal preemption](https://go.dev/src/runtime/signal_unix.go),
[Linux robust ABI](https://www.kernel.org/doc/html/latest/locking/robust-futex-ABI.html).

The next platform slices should be tested across runtimes:

1. Normalize clone/clone3 into one birth request. Model parent TID copyout,
   child-start TID copyout at its correct phase, set_tid_address and logical exit
   clear32+wake-at-address. A join waiter must be woken by address; removing the
   exiting thread's own futex wait record is a different operation. Native error
   and ordering oracles are required; merely enabling clone flags is insufficient.
2. Archive per-task ABI state: clear-tid registration, robust head/length, signal
   mask, alternate stack, pending/start/restart phases. Keep process dispositions
   and process-directed pending signals separate. Reset old carrier registrations
   and install the new logical task's state before running it. These fields must
   survive copy/archive/restore/identity, including the trusted startup boundary.
3. Support guard/reserved mappings, common TLS installation and virtual timed
   waits/readiness. Share one wait/deadline mechanism among futex, sleep and epoll;
   enumerate eligible wake outcomes and runnable task actors.
4. Add robust owner-death from the Linux linked-list ABI, including pending entry
   and owner bits; do not inspect pthread private fields. PI futex remains a
   separate feature. A coherent rseq-absent virtual kernel from exec entry can
   support runtime fallback; full rseq requires a separate CPU/critical-section
   model. Do not grant successful registration and then erase the whole region.
5. Signal mask/altstack restoration is not signal delivery. Delivery needs
   modeled pending events, target selection, frames, rt_sigreturn and blocked-call
   interruption/restart. pthread cancellation and Go asynchronous preemption
   exercise that shared machinery; suppressing their signals changes behavior.

The audit found two prelude shortcuts: a fabricated set_tid_address return and
whole-region rseq masking/live-byte preservation. The TID slice below removes the
former; the rseq behavior still needs independent repair. ABI handlers should apply consistently from the exec stop,
even where the loader prelude otherwise remains a declared trusted environment.

OS-thread fidelity and schedule coverage remain separate acceptance levels.
Capturing all guest memory plus logical task contexts can preserve goroutine
scheduler state without extracting G/P objects. But syscall-only stepping cannot
bound pure computation or expose every memory/atomic/preemption interleaving.
A generic instruction/shared-memory boundary backend, explicit memory model and
reduction strategy are a separate project. Optional runtime observers may name
user tasks; core actors should not silently become goroutines. Disabling GC,
async preemption or lowering GOMAXPROCS defines a restricted workload profile,
not proof for the original configuration.

Use capability-scoped tests, not a blanket pthread-supported flag: first native
raw ABI + glibc/musl create/join/TLS and restore; then mutex/condition/timeouts;
then robust, signals/cancellation, gc Go scheduling/netpoll/GC, and mixed cgo.
A runtime starting or one pthread example passing does not establish exhaustive
model checking for that runtime.

## TID birth and join contract

Flat17/page18 records clear_child_tid and pending CHILD_SETTID state. Parent TID
copyout writes32 bits before the modeled clone completes; child copyout occurs
once, before its first guest instruction. set_tid_address changes only the logical
registration and returns its logical TID. The loader prelude redirects physical
registration to NULL, records the original pointer and returns the same logical
identity as exploration; the114514 shortcut and uncontrolled prelude clone path
are removed. Startup must end before thread birth.

Exit clears32 bits and performs a shared futex wake when another logical task
still uses the address space. The last logical task does not clear; retained host
carriers do not affect this decision. Private/shared waiters are distinct, including
wake/requeue and archives. Failed TID put_user operations do not fail clone; the
whole4-byte writable range is checked first to avoid non-native partial stores.
Read-only file-backed clear-tid wake semantics remain explicitly unsupported.

clone3 validates sizes, extensions and stack arithmetic before birth. Explicit
TID selection, unsupported flags and extended stack address ranges outside the
ordinary lower x86-64 range reject. The completed-syscall model remains
parent-return-first; it does not enumerate child execution before parent return.
exit_group uses fixed slot cleanup order, performing each pending child write
before that slot exits. General backing-based/cross-process shared futex keys and
full termination cleanup are not certified.

`check-thread-tid` runs clone/clone3 x full/pages x normal/tiny-cache oracles,
including persisted pending/registered states as transition sources, shared join,
private-wait isolation, registration replacement/cancellation, last-task behavior,
invalid and cross-page addresses, group termination before child user code, and
clone3 error precedence. `check-runtime-profile` verifies initial-thread logical
identity and disabled physical clear-tid registration. A minimal unmodified glibc
pthread probe still stops at rt_sigprocmask (syscall14); this slice is ABI progress,
not complete pthread support. Its local transcript is in build/tid-pthread-probe/.
