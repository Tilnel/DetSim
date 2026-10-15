# Maintenance baseline

## Closed syscall dispatch slice (2026-10-15)

Invariant: once exploration starts, an unclassified syscall cannot execute a
host-side effect. Explicit passthrough is limited to captured address-space
operations and stable queries of the unchanged owned live process; argument
validation rejects foreign-resource access. Trusted bootstrap/restore injection
remains a separate explicit boundary. Default routing and default route objects
reject; diagnostics name the unsupported operation before executing the kernel.

Lock this with native ptrace tests: an unknown prctl(PR_SET_NAME) cannot change
the child comm; negative/high syscall IDs reject; allowed memory/query calls keep
normal kernel error behavior. Audit existing Redis/Raft traces before changing
routing, then preserve compatible paths through truthful generic emulation:
virtual clocks, a captured seeded random stream, process signal dispositions
(delivery unsupported), disabled-only interval timers, captured umask, deterministic
sysinfo and query-only owned-process limits. No workload-specific exceptions.

All new semantic fields require copy/archive/restore/identity tests and an
incompatible machine snapshot version bump. Fix gettid's fabricated value to the
actual stable owned TID while this backend still requires the same live thread
set. Do not claim logical PID virtualization or complete signals/timers/threads.

Run native policy/side-effect regressions, process-model branch/replay/error tests,
make check, NP1/NP2 default and page gates, Redis exact SET/GET and bounded Raft.
Separate implementation and read-only review. Keep current master workflow and
preserve existing runtime/submodule artifacts.

## Generic framework implementation plan (2026-10-15)

Invariant: core captures machine/environment state without application-specific
fields; properties inspect explicit immutable source/target views and run before
successor publication/dedup. Monitor memory is branch-owned and part of search
identity, not guest restore. Flat/page snapshots share a memory-reader contract;
there is no implicit saved-to-live fallback. Environment choices may inspect an
explicit stopped-live read-only view and return validated outcomes/costs.

Regression first: flat/page read equivalence, explicit expression context and
write rejection; plugin error/ABI handling, state/edge checks, history branch
isolation/archive/identity, decoder noninterference, configurable startup/signal
semantics, no core/example coupling. Preserve before binary/plugins under build.

Passes: (1) common snapshot readers and explicit observation; (2) versioned plugin
API and Raft/Paxos migration; (3) generic monitor product state and single checker
commit boundary; (4) explicit runtime profile and presentation plugins; (5) remove
old fields/pruning/duplicate plugin paths and bump incompatible schemas. No new
dependencies. Separate implementation and read-only review. Keep workload-specific
code in examples, not resource models or persistence. Do not merge two scheduler
implementations without equivalence evidence; route both through the common
transition/check path.

Verification: make check, production NP1/2/3 builds, native restore/state/reader
and plugin regressions, Redis smoke, Raft flat/pages bounded exploration. Compare
same-work baseline where semantics match; report changed exploration coverage
separately, and time only isolated executions after builds finish. Finish with
strict diagnostics, diff review and a coherent commit.

Implementation and validation completed:

- Common `SnapshotView` chunk dispatch (`FlatMemory`/`PagedMemory`) and explicit
  `StateView`: snapshot memory, typed readonly expressions, authoritative clocks,
  logical registers/resource JSON and non-consuming file/pipe/network buffers.
- Public `src/api/plugin.h`, loader/runtime, example adapters and standalone
  public-header builds. No Raft/Paxos/Redis reference remains in product source.
  Pure presentation cannot affect model identity; decoder errors fall back raw.
- Machine snapshots9/10 omit Raft fields. Version2 system metadata owns bounded
  monitor bytes, model compatibility and terminal failure identity. Restore
  selects the complete product state, including history even on same machine.
- Checks precede commit/tree/dedup. Failed monitor writes are discarded; terminal
  states remain inspectable but cannot execute. Native witness regression proves
  valid and failing incoming edges cannot share the wrong counterexample parent.
- Explicit per-node runtime preludes, normalization and signal policy; removed Go
  guessing/private glibc-offset writes. Fixture harnesses declare their own Linux
  ELF bootstrap profiles outside the generic product.

Verification (system GCC14, all exit0):

```sh
make check CC=/usr/bin/gcc CXX=/usr/bin/g++
make NP=2 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4 tracer check-state-view check-plugins check-runtime-profile check-identity check-capture check-io-boundary check-port check-accept check-redis
make NP=1 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4 tracer check-state-io check-page-snapshot check-mappings check-xstate check-futex-snapshot check-futex check-exec check-scenarios
# Repeat NP1 scenarios/mapping/XSTATE/futex and NP2 capture/Redis with verify-pages.
make NP=3 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4 tracer
```

Native scenarios pass14 cases, including a third-system counter plugin with
history/restore/terminal replay and first-violation stopping in BFS/DFS/RAND.
Component tests pass8 cases and10 build contracts. Plugin gates cover both library
adapters and flat/page payloads, ABI/config failures, history/terminal transactions,
pure-decoder noninterference and all generic resource readers. Observer regressions
also cover cold exited-node clocks, symbol reload with changed same-name layout,
readonly ASTs, invalid process qualifiers and lifecycle/poison invalidation.

Strict GCC diagnostics pass on the changed core (existing unused warnings
suppressed); focused new observers/plugin/tests also pass with only the existing
unused-parameter suppression. Selected clang-tidy core/C++ lifetime/move analyzer
checks report no project findings (external-header warnings filtered). Generated
parser changes were reviewed separately from the one-line grammar fix.
Independent read-only review found and verified fixes for monitor commit order,
cold metadata/cache handling, terminal witnesses and telemetry ownership.

Final Raft verification: depth8,3 repetitions per backend/cache,24 old/new runs
all match521 visits/275 new states, budget and protocol checkpoints. Depth12
verify-pages checks19257 captures (23.9GB oracle reads),5593 visits/2867 new states;
paired ordinary pages runs match the same counts,12.517/12.518s old/new and
11.59/11.93s tracer CPU (one run each). Short runs benefit from telemetry removal;
longer exploration is wall-time neutral here with about3% more CPU. See
[measurement scope and full tables](redisraft-benchmark.md).

Remaining limits are tracked in the correctness backlog: fixed NP, supported
non-PIE main-image symbol context, bootstrap outside exploration, thread/signal
lifecycle and wider machine-state closure. Genericization is not a proof of those
unimplemented guarantees. No dependencies or vendor/submodule changes added.

### Bounded telemetry follow-up

The three-repeat comparison exposed 1-second wall-time quantization from the
status thread's fixed sleeps/join. The same thread concurrently reads mutable
state_set/state_tree/runtime structures, which is not safe. Preserve existing
finite-search/violation/cleanup scenario tests and search counts, remove the
background reader, and poll presentation periodically at completed-transition
boundaries on the exploration thread. Status display must not delay exploration
completion or observe half-published state. Recheck counts and remeasure before
claiming an improvement; keep the pre-telemetry comparison separately.

## Joint virtual I/O boundary plan (2026-10-15)

Lock mixed file/socket/pipe/epoll read-write-close, flags, guest-copy failure,
TCP zero-write/EOF, epoll opaque data and resource archive behavior before edits.
Remove unused host-mmap helpers and success-only sync/fcntl stubs; guest addresses
must never be passed to tracer mmap/munmap. Make the existing per-process shared
FdManager track kinds/status/descriptor flags deterministically, rather than
pretending UNKNOWN is useful type tracking. Keep resource payloads in FS/net.

Consolidate descriptor syscall dispatch in one small function boundary, not a new
framework. Copy input before mutation; commit offsets/queues only after successful
output transfer. Include omitted pipe and pre-accept TCP state and epoll userdata
in archives. Pipe IDs derive from captured state, not a static host counter.
Represent deterministic stdin EOF and captured stdout/stderr in the existing FS
payload model; no live terminal passthrough. Reuse checks for close/invalid flags.

Changing semantic FD metadata and resource archives requires flat format7/page
format8; reject5/6 explicitly before decoding. Preserve old runtime stores, do not
migrate/delete them. Test copy/archive/restore/identity plus actual ptrace entry
blocking, ephemeral native programs and existing Redis/Raft workloads. Keep dup,
blocking wakeups, advanced epoll and cross-node filesystem semantics explicit
follow-ups; do not report a complete Linux I/O implementation.

Verification: the initial joint fixture failed16 cases before implementation;
extra UDP dispatch and full TCP endpoint regressions independently failed before
their fixes. `make check` passes8 component cases and10 build contracts. New
archive tests reject16 malformed descriptor/resource graphs. Ordinary and
verify-pages NP2 capture oracles preserve queued TCP/pipe data, flags, epoll data
and recapture identity after repeated restore with ordinary/tiny caches.

Final commands (system GCC14, all exit0):

```sh
make check CC=/usr/bin/gcc CXX=/usr/bin/g++
make NP=1 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4 tracer check-state-io check-page-snapshot check-mappings check-xstate check-futex-snapshot check-futex check-exec check-scenarios
make NP=2 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4 tracer check-io-boundary check-port check-accept check-identity check-capture check-redis
# Repeat NP1 gates and NP2 capture/Redis with DETSIM_SNAPSHOT_BACKEND=verify-pages.
make NP=3 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4 tracer
python3 tests/page_snapshot_benchmark.py --repeats 1 --depth 12 --output build/io-boundary-benchmark-quiet
```

Strict GCC `-Wall -Wextra -Werror -fsyntax-only` checks pass for state/FD/FS/net,
common I/O and new tests. Guest/dispatcher/handlers also pass with the existing
unused-parameter warnings disabled. No separate C++ lint framework is configured;
`git diff --check` is clean. Native mmap error regression first reproduced an
unsigned-return-value bug, then passed after checking a signed syscall result.
Unused host-mmap helpers, duplicate FD dispatch/flag storage and cached epoll
notification queues were removed. Changed code is in guest transfer, state format
and graph validation, syscall dispatch, `subsys/io`, `subsys/fs`, `subsys/net`, and
the corresponding Makefile/test/documentation gates.

## Page snapshot branch plan (2026-10-15)

Branch: `experiment/page-snapshots`, based on `08ed190`.
Introduce an opt-in v6 manifest referencing independently content-addressed pages,
using the existing StateStore and barrier. Keep v5 capture as the default oracle.
Full capture reads all supported regions; previous page buffers are only exact
byte-equality hints. Both formats use one validated SnapshotView and restore
only differing pages after comparing live bytes. No atom/search changes.

Acceptance: version-local identity and exact decoded v5/v6 equivalence, cold/tiny
cache and reopen, missing/corrupt page rejection, store failure invalidating warmed
views, RO/remap/rseq and sparse restoration, unchanged Raft checkpoint/counts.
Measure full-chain wall/CPU/RSS/bytes for the original binary, v5 on this branch,
v6 full reads, and separately explicit soft-dirty delta. Do not compare generic
store save/new counters as if pages were complete child snapshots. Unsupported
tracking remains opt-in; no asynchronous-writer proof is inferred from fixtures.

## Soft-dirty experiment plan (2026-10-15)

This is an opt-in experiment, not a supported incremental snapshot backend.
Keep full snapshot format, identity, restore and search unchanged. With the mode
unset, retain the ordinary capture path without tracking allocations or syscalls.
Use per-live-process raw memory baselines, pagemap bit55 and clear_refs=4; map
layout changes and missing baselines force full reads. Restore writes remain
inside the tracking epoch. Verification mode compares reconstructed bytes against
full reads before normalization and rejects omissions. No asynchronous-write
coverage claim follows from the finite fixtures.

Acceptance: real ptrace fixtures cover user/kernel/tracer writes, readonly writes,
discard/replacement and partial pages; existing capture byte oracles pass. Fixed
Raft seed/depth/checkpoint/visit counts must agree in full/verify/delta modes.
Measure sequential alternating full/delta runs, all-thread/main/tracee CPU, total
DFS wall time and tracking costs/bytes; report slowdown as readily as improvement.
Do not promote the mode to default or redesign persistence based on this experiment.

Status: maintenance scope and the functional repair pass below are implemented and reviewed.
The initial maintenance scope was reproducible development and truthful guidance,
without changing runtime semantics. The later functional pass explicitly extends it.

## Cleanup plan (before implementation)

1. Missing tests: establish a small, dependency-light regression suite for existing
   subsystem behavior. Repair the DWARF test entry if it can use the real interfaces
   without fake runtime layouts. Preserve local experimental tests separately.
2. Build reliability: propagate compiler failures, make compiler selection explicit,
   prevent stale objects across NP/build-mode changes, and provide one check entry.
   Test these build contracts before modifying the recipes.
3. Conflicting guidance: replace the duplicated, inaccurate assistant guide with a
   pointer to a short AGENTS.md and an evidence-based development document.
4. Missing ownership of correctness: record active call paths, known defects, and
   the exact regression evidence required before expanding or optimizing them.
5. Verification: run regression tests, a fresh production build, scoped static
   checks, and an independent review. Record limitations rather than claim that
   passing component tests establishes model-checker correctness.

## Constraints

- No new dependencies, bulk formatting, source-directory moves, or semantic rewrite.
- Do not modify vendored code, submodules, user snapshots, or unrelated local files.
- New source and test files must be visible to Git; build/runtime output must not be.
- Keep one authoritative guide; historical reports are evidence, not current status.
- Core semantic fixes require focused failing tests in subsequent passes.

## Acceptance

- A documented command runs useful tests without ptrace or changing host ASLR.
- A failing compiler fails the build; changing NP/mode cannot silently reuse objects.
- Future work starts from a concise map and a prioritized backlog with acceptance tests.
- Verification results distinguish build/component coverage from end-to-end coverage.

## Results (2026-10-14)

- Replaced duplicated compile recipes and timing wrappers. Direct compiler recipes
  propagate failure; NP/mode directories and content stamps invalidate stale objects.
  PGO phases rebuild via the stamp rather than racing a clean prerequisite.
- Added 10 build-contract tests against the real Makefile. Before edits, the initial
  6 tests failed, exposing hidden compiler failure and stale-configuration behavior.
- Added 4 component cases for FdManager and FutexState: allocation, copy isolation,
  cereal archive round trips, wait/wake masks, handler requeue and range wake.
- Kept local DWARF experiments intact but removed their unsafe fake runtime from the
  default test entry. They require a separate repair, not relaxed compiler checks.
- Consolidated contributor guidance; annotated the historical refactoring report.
  Exposed the existing `src/segv_handler.hpp` required by `main.cpp` to Git without
  changing runtime behavior (trailing whitespace normalized before commit). No
  submodules or existing snapshots were modified.

Verification:

| Command / check | Result |
| --- | --- |
| `make check CC=/usr/bin/gcc CXX=/usr/bin/g++` | PASS: 4 component cases + 10 build tests |
| Component tests with `-O2 -DNDEBUG` | PASS; checks remain active (deliberate failed-check mutation exits 1) |
| Component ASan + UBSan run | PASS with `ASAN_OPTIONS=detect_leaks=0`; LeakSanitizer unsupported in this sandbox |
| New test C++ `-Wall -Wextra -Werror -fsyntax-only` | PASS |
| New test C++ `clang-tidy -checks=-*,clang-analyzer-*` | No enabled analyzer findings; tool reports suppressed compiler warnings |
| New test C++ `clang-format --dry-run --Werror` | PASS |
| Python `py_compile`, local documentation links, `git diff --check` | PASS |
| `make NP=1 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4` | BLOCKED: old libdwarf API vs installed 0.11.1; C10 |
| Unqualified `make check` in this environment | BLOCKED: custom PATH GCC 15.2 pthread headers; select `/usr/bin/g++` |

Existing production warning: FdManager does not use its `type` parameter. Full
tracer execution, PGO workload correctness, and exploration equivalence were not
validated. The passing gate deliberately states its component/build scope.

Independent review found no outstanding must-fix maintenance regressions. Its LSP
batch was aborted after hanging and is not validation evidence; direct compiler
and analyzer checks above provide the scoped C++ evidence.

## Functional repair pass (2026-10-14)

Plan before implementation:

1. Unblock C10 using the supported libdwarf API and real multi-compilation-unit
   fixtures linked to production code. Do not recreate the legacy mock runtime.
2. Reproduce C11's inconsistent futex queue/index through the public helper; fix
   only the demonstrated invariant and test archive round trips and subsequent wake.
3. Run a minimal native workload through the actual tracer in a disposable working
   directory/session, with timeouts. Compare single stepping and restore/replay;
   fix demonstrated failures such as filename-dependent startup (C05).
4. Measure a repeatable workload before proposing a performance change. Prefer
   removing unnecessary work after tests prove equivalent outcomes.
5. Independently review production changes, run component/build/DWARF/scenario
   gates, and update open/closed backlog items with the actual verification scope.

This pass may uncover additional defects; add their reproductions before fixes.
Do not claim general multithread correctness from a single-thread workload.

### Functional repair results

- C10: production DWARF API migration; real multi-CU DWARF 4/5 parser fixture.
- C05: removed filename-based startup classification and fixed Go ELF section-name
  lookup. Ordinary and Go-section fixtures plus existing gc-built binaries classify
  correctly; this is not a Go runtime support claim.
- C11: requeued waiters retain a correct thread→address index through serialization
  and subsequent wake. Same-address/complete syscall semantics remain open.
- C08 (partial): populate every RegionInfo byte. Before the fix, replaying the same
  edge produced different hashes; afterward, three repeated restores preserve both
  the counter and successor hash.
- C09: normal and SIGTERM shutdown terminate/reap only owned children. An unrelated
  same-group sibling survives; a signal handler no longer acquires storage locks.
- C12/C13/C14: preserve exited status, return from BFS on an invariant violation,
  and preserve batch command arguments. Each has a failing real scenario before
  the fix and a passing one afterward.

Performance evidence: the restore/replay scenario under `strace -e trace=madvise`
issued 63 MADV_SEQUENTIAL calls before removal, all failing with ENOMEM. After
removing the loops acting on remote addresses in the tracer's address space, it
issues zero. The same counter/hash regression passes. Observed 0.1463s→0.1215s
includes strace/CLI overhead and single-sample noise; it is not a throughput claim.

Verification commands (GCC 14.2 / libdwarf 0.11.1):

```sh
make NP=1 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4 tracer check check-integration
python3 tests/tracer_scenarios.py
python3 tests/tracer_scenarios.py --profile TracerScenarios.test_restore_and_replay_preserve_counter_and_state_identity
```

Results: production compile/link, 5 component cases, 10 build contracts, DWARF/ELF
integration fixtures and 9 real ptrace scenarios pass. Component ASan/UBSan passes
with leak checking disabled for the ptrace-constrained sandbox. New fixture sources
pass clang-format; Python compilation and git diff whitespace checks pass.
Independent review found no outstanding must-fix issue in the scoped changes.

Static-analysis limits: installed clang-tidy 15 cannot parse the installed GCC
14/15 standard-library ranges/format headers on several production translation
units, so that run is not a clean-analysis claim. GCC 14 `-fanalyzer` completed
`futexstate.cpp` and `utils.cpp` without diagnostics; the much longer lifecycle
translation-unit analysis was interrupted. All changed production units did pass
the actual GCC production build. This is scoped evidence, not a whole-project audit.

Remaining limits are recorded as open backlog items: multi-thread snapshot/futex
semantics, general state identity and eviction, missing standard-stream modeling,
loader CPU normalization, and workload-specific Raft pruning. Existing scenarios
are single-thread, one-node, non-PIE; they do not validate complete syscall fidelity.
API migration references: installed libdwarf 0.11.1 header and
[official CU traversal documentation](https://www.prevanders.net/libdwarfdoc/group__compilationunit.html).

## Redis blocker repair plan (2026-10-14)

1. Reproduce accept with a genuine remote sockaddr/length buffer; repair checked
   copy-in/copy-out, returned length, truncation and invalid-pointer behavior.
2. Reproduce the large-DWARF forward-type lookup on a small multi-CU fixture; remove
   reentrant CU scans/unchecked growth without weakening symbol/type validation.
3. Add a bounded, reproducible Redis manual-schedule runner. Require client SET/GET
   response checks and normal virtual exit, not merely server readiness messages.
4. Verify the original debug binary as well as the earlier diagnostic copy; record
   any later memory, TCP or scheduling defects exposed rather than hide them.
5. Run existing component/integration/native gates and independent review. Keep
   Redis/vendor source and user snapshots unchanged. Use isolated build directories.

### Redis repair verification

- `sockstate.cpp`: 12 real cross-process accept cases pass, including genuine
  inaccessible-in-parent pointers, truncation/length writeback, invalid/readonly
  memory, fd collision and stale-first/live-second queue progress. The old code
  crashed on the first remote pointer case; the follow-up stale-head fixture also
  failed before its fix.
- `dwarf.cpp`: removed repeated/reentrant CU scans in favor of one definition
  index; incomplete same-name aliases cannot recursively resolve themselves.
  Ordinary, forward and class fixtures pass under DWARF4/5. The same-name fixture
  previously crashed; the pointer-to-class fixture caught an index omission.
- `tests/redis_scenario.py`: original debug Redis (no stripping) completes SET/GET,
  exact RESP assertions and virtual exit0, then tracer exit0. The root `check-redis`
  target builds NP=2 and preserves bounded per-run artifacts for reproduction.
- Final production parser check on original Redis: 0.368s, peak RSS 65,688 KiB,
  cap1GiB / timeout20s. This replaces the previous >45s / ~6.2GiB failure, not a
  claim about general search throughput.
- Existing 5 component cases, 10 build contracts, ELF/DWARF integration and 9
  NP=1 native scenarios pass. Original Redis full path also passes with new code.
  Independent review found and rechecked stale-queue and class-index regressions;
  no remaining must-fix finding in this scope.

Model limits remain explicit in `redis-smoke.md`: ambiguous same-fd peers are
rejected, EFAULT retains pending connection, partial guest copyout is not rolled
back, stdout simulation and general multithread/restore/search correctness remain
outside this Redis success claim. Vendor source and user snapshots were not changed.

The bounded GCC `-fanalyzer` pass on the two larger production units hit its
25-second limit and emitted library-allocator leak warnings without a confirmed
application allocation path, so no static-analysis-clean claim is made. The
instrumented DWARF parser also passed the original Redis input with
`ASAN_OPTIONS=detect_leaks=1` under the unrestricted runner (no reported leak).
Compiler builds,
regressions, DWARF ASan/UBSan runs, new-test formatting, Python syntax and diff
checks provide the verification evidence above.

## State/atom contract repair pass

Plan: first turn the demonstrated XMM failure into a regression and capture user
XSTATE per thread, bumping/rejecting incompatible snapshot versions. Fix current
syscall inputs before entry dispatch. Use one completed syscall per automatic
transition as the conservative reference boundary, rather than silently consuming
CKPT_NO loops. Require complete snapshot I/O and reject missing/invalid state.

Next replace XOR system identity with an ordered, versioned hash of exactly the
serialized semantic metadata (node hashes, status and error budget), protected by
permutation/status/budget tests. Remove host RNG from TCP port allocation using
serialized socket state. Keep mapping/thread/time semantics outside those fixes
explicit until their own regressions exist. Add nonzero top-level error handling
so restoration failure cannot be mistaken for a valid successor; preserve old
snapshot files and reject them rather than mutate/migrate them in place.

Mapping follow-up was required because strict saved-memory reads exposed missing
read-only literals. Capture now includes all private readable regions; restore
preserves permissions and backing, and rejects unsupported mappings. Canonical
adjacent VMAs prevent restore-induced split/merge hash drift. Unchanged memory
regions skip copy/protection changes. A reserved scratch VMA that expands into
application memory is rejected, never silently omitted.

Verification: component/build-contract and DWARF/ELF gates; XSTATE, exact I/O,
mapping, syscall-boundary, ordered-identity and TCP-port regressions; native
restore/replay scenarios and Redis SET/GET. Independent review required explicit
monitor shutdown during search exceptions as well as scratch-overlap rejection.
Remaining semantic gaps are tracked in correctness-backlog.md; this pass does not
establish multithread exploration completeness or virtualize time/signals.

Final commands (all successful, GCC 14 via explicit compiler paths):

```sh
make NP=1 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4 check check-integration check-xstate check-exec check-state-io check-mappings
make NP=1 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4 tracer check-mappings check-state-io
make NP=1 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4 check-scenarios
make NP=2 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4 tracer check-identity check-port check-accept check-redis
python3 -m py_compile tests/tracer_scenarios.py tests/redis_scenario.py
git diff --check
```

The final native suite has 11 scenarios, including repeated SIMD restoration and
BFS/DFS/random-search failure cleanup. Accept has 12 cases; the final Redis client
and tracer both exit 0 after exact SET/GET replies (7.65 seconds on this host).
Touched state code passed warning checks; the scheduler retains its existing unused
variable/parameter warnings. No whole-project static-analysis-clean claim is made.

## Futex branch-state repair

Invariant: waiting queues, thread blocked status and selected actor must describe
the same saved state. Copying or exploring a wake/requeue branch must not mutate
the source, and restoring must reconstruct eligibility before execution.

Plan: regress copy/archive/capture/replay first; replace owning raw futex pointers
with value ownership, derive blocked status from queues, preserve thread slots
for unchanged thread sets and reject unsupported lifecycle restore. Correct wake
selection and requeue ABI with syscall regressions; waiting becomes a saved
successor. Keep unsupported timeout/PI paths explicit. Bump snapshots to v5 to
reject older archives that omitted live queues. Test selected-actor publication
when automatic execution skips a non-runnable slot. No scheduler consolidation.

Results: queues use optional value ownership and archive loads clear stale queues;
virtual slots and creation metadata survive capture. Restore rejects changed live
thread sets before mutating memory, eliminating speculative recreation and positional
register assignment. Same-hash restore still restores status and model metadata.
REQUEUE paths are unified; WAIT is a suspended successor, WAKE updates removed
waiters only. Dormant madvise handler no longer touches tracer memory or fabricates
wakes. FIFO wake selection and complete actor enumeration remain unimplemented.

Validation commands (GCC 14, all successful):

```sh
make NP=1 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4 tracer check check-exec check-state-io check-futex check-scenarios
make NP=1 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j3 check-futex-snapshot check-mappings check-xstate
make NP=2 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4 tracer check-identity check-port check-accept check-redis
make NP=1 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4 check-scenarios check-futex-snapshot
git diff --check
```

Evidence: six component cases, ten build-contract checks, thirteen ptrace futex
cases, six execution-boundary cases, eleven native tracer scenarios, repeated
snapshot restore and fresh-process disk read, existing mapping/XSTATE/I/O/identity/
network gates, and Redis exact SET/GET (both client/tracer exit0, 7.65s).
Independent review additionally checked Linux wake/requeue behavior experimentally.
Touched units compile; warning checks retain existing unused parameters, so no
whole-project lint-clean claim is made. Disk test uses shutdown/join as durability
barrier; a discovered wait_for_completion race remains C26, not silently waived.

## Storage subsystem repair

Scope: StateStore cache/worker layer, StateStorePacked snapshot log/index, and
SysStateStore metadata. Required invariant: successful save followed by completion
barrier permits exact fresh-process reload; every load path returns identical
immutable bytes; eviction/prefetch/dedup cannot change identity or drop pending
writes. Short I/O, corruption and asynchronous errors must fail explicitly.

Plan: regression tests before each defect repair; remove duplicate mutable cache
state and unsafe maintenance paths, retain existing public callers and compatible
validated records where possible. Account queued and executing writes atomically;
publish data before indexes and flush before reporting persistence. Bound cache
payload, keep returned buffers owned independently, and test shutdown/reinit.
Measure the same 128 x128KiB mixed-compressibility dataset with hot and constrained
caches before/after; byte-for-byte checks are mandatory in every benchmark pass.
Do not infer throughput gains from a weaker pre-fix completion barrier.

Storage implementation results: removed mutable cross-thread cache entries and
eviction worker, duplicated cache read paths, unsafe packed compaction, and
metadata idx/inc publication. Returned buffers own immutable bytes. Pending writes
are bounded and retained through active work; errors poison further operations.
Persistent descriptors and append logs eliminate repeated opens and full-index
rewrites. Metadata has a bounded lazy cache, shared read-only scanning, and updated
query/verification tools. Index recovery publishes recovered keys before append.

Baseline `576eb3e` was independently extracted and compiled with GCC14 release
flags and the same benchmark source; no new headers/objects were mixed in.
Byte-checked benchmark medians on this host (5 runs each; old pressure3):

| Workload / measure | Before | After | Interpretation |
| --- | ---: | ---: | --- |
| Hot cache, 128x128KiB save+barrier | 177.240 ms | 60.621 ms | 2.92x faster; new barrier provides stronger completion semantics |
| Hot cache,4 complete read passes | 9.033 ms | 5.238 ms | 1.72x faster |
| Hot cache duplicate saves | 3.116 ms | 2.631 ms | byte comparison retained |
| Hot process peak RSS | 48,092 KiB | 56,080 KiB | increased; no general RSS reduction claim |
| Metadata10,000x32B save+flush | 119.943 ms | 15.098 ms | 7.94x faster |
| Metadata4 read passes | 1.535 ms | 1.718 ms | modest slowdown |
| Metadata peak RSS | 14,440 KiB | 14,500 KiB | bounded cache bookkeeping has overhead |
| Constrained cache | 1/3 valid runs | 5/5 valid runs | old2 wrong-byte failures followed by SIGABRT; no valid speedup comparison |

New constrained-cache median save/barrier55.073ms, read41.043ms, dedup11.233ms,
RSS30,200KiB. Its disk duplicate checks intentionally read/compare full bytes.
Old one successful constrained run is not a reliable performance baseline.
Packed syscall regression:1000 saves issue0 fsync; explicit barrier issues4.
These are microbenchmarks, not search-throughput guarantees or hard RSS bounds.
All runs use separate disposable stores; raw logs/results in ignored
`build/storage-benchmark`, reproducible harness in tests/storage_benchmark.*.

Verification (explicit `CC=/usr/bin/gcc CXX=/usr/bin/g++`, all successful):

- NP1 `check-storage`:10 cache groups, packed raw/compressed/fault/recovery/rotation
  gate, metadata concurrency/collision/cache/fault gate, read-only tool scenarios.
- NP1 `check`:6 components and10 build contracts; `check-scenarios`:12 actual
  tracer scenarios including fsync failure exit1 and tracee cleanup.
- NP1 `check-futex-snapshot check-state-io check-mappings check-xstate`.
- NP2 production build and `check-identity check-port check-accept check-redis`;
  Redis exact SET/GET and normal exits pass (final run9.14s).
- Strict compiler warnings on storage units; packed real GCC analyzer passes;
  metadata ASan/UBSan/LSan passes. Cache/metadata full analyzer reports allocator/
  standard-template diagnostics; no whole-project static-analysis-clean claim.
- Cross-layer review found and fixed recovered-index publication and atexit
  singleton lifetime. `git diff --check` and Python syntax validation pass.

Remaining limits: no cross-store transaction, no arbitrary power-cut recovery
proof, partial/corrupt files are rejected instead of automatically repaired,
indexes still grow with state count and metadata startup scans the log. Shared
buffers held by callers and one oversized pending state can exceed cache budgets.

Verified benchmark result files: `before-verified-results.json` and
`after-verified-results.json` under `build/storage-benchmark/`. RSS includes the
benchmark's generated payloads and runtime, not only store-owned memory.

## Conservative snapshot capture optimization

Invariant: optimized capture produces exactly the same child blobs/system identity
as fresh full capture, including inactive memory changed outside model metadata.
Investigation found inactive metadata comparison always mismatched brk=0, while
its source was stale runtime bookkeeping after branch restore. Do not repair this
by assuming stopped-process bytes cannot change: cross-process writes, shared VM
descendants and asynchronous kernel I/O make that unsafe with current emulation.

Plan: retain full memory/maps/brk/GP/XSTATE capture; pass explicit restored source
identity as a dedup hint only. Delete dead incremental comparison and full old-state
copy. The store may compare only an already-hot/pending immutable candidate before
hashing; cold hints follow the ordinary path without speculative disk reads. All
store preconditions/error behavior remain enforced. Full/verify modes and a real
NP2 branch-replay oracle cover changed memory and modeled cross-node state.
Measure the same fixed-seed Raft depth12 workload against saved baseline binary,
with both small and large caches, after equivalence gates pass.

Result: introduced store-owned resident reference comparison without bypassing
read-only/poison/length/lifecycle checks. All snapshots still capture full bytes.
Cold hints have no additional reads; active nodes retain the ordinary save path.
The source reference never authorizes metadata-only memory reuse. Diagnostics
`DETSIM_CAPTURE_MODE=full/verify` and `reference` statistics expose the comparison.

Paired fixed-seed Raft depth12,3 runs per binary/cache profile, all 3433 visits,
1776 reported new states,11127 save requests,403 new snapshots and identical
protocol checkpoint results. L1/L2 32/64MiB: save median7.872→7.202s (-8.5%),
DFS14.028→13.029s; 256/512MiB: save6.360→5.879s (-7.6%), DFS10.889→9.881s.
The wall-time reduction includes monitor's one-second quantization; memory is
essentially unchanged. See redisraft-benchmark.md for caveats and raw result paths.

Verification (explicit GCC14 compiler paths):

- NP2 production build: `check-capture check-store-cache check-identity check-port
  check-accept check-redis` all pass. Capture oracle runs with resident and1-byte
  caches; external inactive memory and socket/time changes match full capture.
- NP1 production build: `check check-storage check-scenarios check-futex-snapshot
  check-state-io check-mappings check-xstate` all pass (12 native scenarios).
- NP3 real Raft depth12 additionally passes full and verify modes: both retain
  3433 visits/1776 new states/403 new snapshots; verify extra ordinary saves raise
  calls to18545, with no hash disagreement. Full mode reference count is0.
- `-Wall -Wextra -Werror -fsyntax-only` state.cpp, state_store.cpp and capture
  test succeeds; Python syntax and git diff checks pass; independent review found
  no remaining correctness blocker in this bounded change.

Future large gains still require proven dirty tracking or an enforced closed
execution profile. Stopping one thread does not prove its memory cannot change.

## Closed syscall slice verification

Completed on master with `/usr/bin/gcc` and `/usr/bin/g++`:

- `make check`: component and build-contract gates.
- NP=1: `check-exec check-state-io check-mappings check-xstate
  check-futex-snapshot check-futex check-page-snapshot check-scenarios`;
  nine exec/ABI cases and fourteen native scenarios. Existing madvise/shared-map
  expectations now assert earlier rejection and preserved state.
- NP=2: `check-syscall-policy check-process-state check-state-view
  check-runtime-profile check-identity check-capture check-io-boundary
  check-accept check-plugins check-redis`. Capture covers both snapshot backends;
  Redis fixed-schedule SET/GET completes. Initial policy RED had nine failures.
- NP=3 production build and depth8 Raft-library benchmark, one round each:
  flat and pages both 2,582 visits / 1,347 new states, respectively about
  396 and 587 states/s. Artifacts: `build/closed-syscalls-raft` and
  `build/closed-syscalls-raft-pages`. Single runs are smoke measurements,
  not evidence of performance improvement or equivalence to the old host inputs.

Separate read-only review found no remaining blocker in this slice. Injection
failure on an unexpected signal is explicit but lacks a dedicated signal-injection
regression. Thread lifecycle, general signal delivery and complete enabled actor
enumeration remain separate work.

## Parked terminal branch restoration (2026-10-15)

Invariant: a virtual-exit or intercepted-fatal node retains an owned stopped OS
image. Loading its state and continuing a live peer must use that branch's image
and resources, not the last live branch left in the OS process. Truly dead OS
tasks reject; physical thread creation/exit reconstruction remains unsupported.

The initial NP=2 production-linked regression failed with `exited node retained
another branch's memory`. The fix removes the alive-only conditions from both
persisted child loading and physical-image restoration, and shares stopped-thread
validation with the same-hash path. The transition entry rejects invalid/terminated
actors before restore or execution. Kernel-model v2 invalidates old semantic
contexts; flat11/page12 layouts are unchanged.

Verification with `CC=/usr/bin/gcc CXX=/usr/bin/g++`:

- `make check`: 8 component cases and 10 build-contract tests.
- NP=2 `check-capture`, default and `DETSIM_SNAPSHOT_BACKEND=verify-pages`, normal
  and tiny caches: real virtual exit, live peer transition, SIGILL fault, persisted
  reload, memory/RNG/umask/time branch isolation, recapture/predecessor identity,
  terminated-actor rejection and actual physical death rejection.
- NP=2 `check-identity check-state-view check-plugins check-runtime-profile
  check-process-state check-redis` under verify-pages; Redis SET/GET completes.
  Identity fixtures now use valid manifests and reject missing terminal images.
- NP=1 `check-scenarios check-exec check-futex-snapshot check-futex
  check-state-io check-page-snapshot`: all pass, including 14 native scenarios.
- Strict `-Wall -Wextra -Werror -fsyntax-only` on both changed C++ tests.
- NP=3 production build and one depth8 Raft run per backend. Both visit 2,582
  states / 1,347 new states. Flat single-run throughput was about394 states/s
  versus the preceding smoke's396; no statistically controlled performance claim.
  Artifacts: `build/terminal-restore-raft{,-pages}`.

Separate read-only review found no blocker. This closes the reproduced C08 parked
terminal branch leak, not C02 lifecycle reconstruction or C03/C25 scheduling.
