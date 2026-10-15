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

## Logical threads and stopped-image prerequisites

Invariant: host thread IDs are execution bindings, not serialized logical
lifecycle. Archives carry explicit liveness and logical parent IDs; local runtime
provenance resolves bindings on restore, including disk reload. Foreign execution
images reject before any node changes. Removing host IDs from metadata does not
normalize their occurrences in guest registers, TLS or arbitrary memory.

Before capture injections/memory reads and before restore, all owned threads must
be ptrace-stopped and the physical set must match. The first gate verifies rather
than asynchronously interrupting arbitrary running guest code. Keep unsupported
creation/exit rejected until carrier reconciliation is implemented and tested.

Acceptance: archive binding invariance and explicit liveness, metadata-only reload
without carriers, same-run persisted restore, foreign-source rejection without
partial mutation, and a real pthread clone fixture with running-worker refusal
and successful revalidation after stopping it. Run component/build and NP1/NP2
native gates, then review before expanding lifecycle support.

Thread prerequisite verification: old standalone archive regression reproduced
physical-TID identity leakage. NP1 `check-thread-control check-thread-identity
check-thread-carrier check-futex-snapshot check-exec check-scenarios` passes,
including real pthread running-worker refusal, raw carrier creation and14 native
scenarios. NP2 capture/identity/state-view/plugins/runtime-profile/syscall-policy/
process-state/Redis gates pass with verify-pages and tiny-cache capture. `make
check` passes. The initial duplicate-check implementation measured358states/s;
private prevalidated helpers and shared binding layouts improve the same depth8
Raft smoke to383states/s (2,582 visits / 1,347new), compared to prior394. These
single samples do not establish statistical performance equivalence. Carrier
creation is tested infrastructure; rawclone/exit dispatch remains disabled in
this prerequisite commit. Failed internal carrier creation requires abort.

## Raw clone/exit carrier integration (2026-10-15)

Implemented a runtime carrier pool over the verified creator, with deterministic
logical getpid/gettid/clone results, logical exit codes, explicit actor selection,
canonical frozen-main headers and spare-carrier exclusion. Replaced dormant real
clone/exit handlers with one modeled path. Flat15/page16 and kernel model v4 reject
prior execution contexts. No vendor code or dependencies changed.

The initial extended negative clear-tid test exposed a failed-step rollback bug:
a source hash still looked current after the guest reached a rejected syscall.
Runtime dirty flags now invalidate before resume and remain set through failures;
only successful publication clears them. A restore at syscall entry first skips
the pending call with orig_rax=-1; capture rejects entry before injection/storage.
The global running-state alias also detaches before clone mutates its source.

Validation with `/usr/bin/gcc` and `/usr/bin/g++`:

- `make check`: eight component cases, ten build-contract tests.
- NP1 `check-thread-lifecycle`: full/pages x normal/tiny cache, real A/B birth,
  A exit, C birth, four logical slots over three physical tasks, TLS/IDs,
  persisted restore/recapture and replay; main exits77 while peers continue and
  last worker exits0. Negative clear-tid/rseq/robust ownership and entry rollback
  paths are native, including no guest clone escape and source-alias replay.
- NP1 thread-control/identity/carrier/futex-snapshot/exec/scenarios/state-io/
  mapping/page-snapshot gates pass; native scenario suite has fourteen tests.
- NP2 verify-pages capture/identity/state-view/plugins/runtime-profile/
  syscall-policy/process-state/io-boundary/accept/Redis gates pass. Redis SET/GET
  completes. The identity test separately checks exit-code archive/identity.
- GCC `-Wall -Wextra -Werror -fsyntax-only` on new pool and lifecycle/identity
  tests; `git diff --check`; independent read-only review passes.
- NP3 production build, one depth8 Raft smoke per backend: both2,582 visits and
  1,347new states. Flat about375states/s, pages507states/s. Historical earlier
  single samples were394/587; these runs do not prove performance equivalence
  and include new validation overhead. Artifacts `build/thread-pool-raft{,-pages}`.

Scope remains raw same-address-space threads. TID writeback/clear-tid modeling,
pthread/rseq/robust owner-death, full process teardown, automatic exhaustive actor
selection and instruction-level shared-memory boundaries remain open. A raw birth
requires zero actual clear-tid/rseq registration; nonempty robust ownership rejects
exit. Internal creation failures abort rather than pretending a guest errno.

## TID/clear-tid verification (2026-10-15)

Completed the generic clone/clone3/set_tid_address ABI slice. Native probes under
build/tid-abi-probe established ignored bad TID copyouts,32-bit writes, atomic
cross-page faults and clone3 size/error rules. The pre-fix component regression
reproduced shared wake incorrectly removing a private waiter.

With CC=/usr/bin/gcc and CXX=/usr/bin/g++:
- make check:9 component cases and10 build-contract tests pass.
- NP1 check-thread-tid:8 clone/clone3 x full/pages x cache variants pass;
  check-thread-lifecycle, check-futex, check-futex-snapshot, check-exec and
  check-scenarios pass (14 native scenarios).
- NP2 verify-pages capture/identity/state-view/plugins/runtime-profile/
  syscall-policy/process-state/io-boundary/accept/Redis gates pass.
- Strict GCC -Wall -Wextra -Werror syntax checks pass for the new pool/TID and
  startup tests. Independent read-only review and git diff --check pass.
- NP3 production build and bounded depth8 Raft run per backend both produce
  2,582 visits/1,347 new states; single-run full381/pages547 states/s. Artifacts
  build/thread-tid-raft{,-pages}; these are smoke samples, not statistical claims.

The unmodified minimal pthread probe in build/tid-pthread-probe stops explicitly
at syscall14, rt_sigprocmask. No pthread/private-runtime state patches were added.

## Per-task signal mask verification (2026-10-15)

With CC=/usr/bin/gcc and CXX=/usr/bin/g++:
- `make check`: 9 component and 10 build-contract tests pass.
- NP1 `check-thread-signal-mask` and `check-thread-tid`: eight variants each pass.
  Thread control/carrier/identity/lifecycle, futex/futex-snapshot, exec and the
  fourteen native scenarios also pass.
- NP2 capture/identity/state-view/plugins/runtime-profile/syscall-policy/
  process-state/io-boundary/accept/Redis gates pass. Startup inheritance and
  prelude mask updates use the production dispatcher path.
- GCC `-Wall -Wextra -Werror -fsyntax-only` checks cover control/carrier/pool and
  signal-mask/TID fixtures. Independent read-only review passes.
- NP3 production build; depth8 Raft smoke produces 2,582 visits and 1,347 new
  states for both backends. Full375/pages491 states/s, compared with prior single
  samples381/547. Additional pending/mask validation has overhead; these samples
  do not establish a statistically measured regression or performance equivalence.
  Artifacts: build/thread-sigmask-raft and build/thread-sigmask-raft-pages.
- Actual unmodified pthread probe now passes syscall14 and explicitly stops at
  the ordinary-readable-private mapping restriction (guard mapping support is
  still open). Artifact: build/tid-pthread-probe/signal-mask-transcript.log.

No workload-specific state extraction was added. Pending signal queues, delivery,
altstack, cancellation and Go asynchronous preemption remain unsupported.

## Anonymous guard verification (2026-10-15)

Red: the new mapping fixture failed at capture of its explicit PROT_NONE range.
The implementation adds only snapshot-specific hidden-memory reads; ordinary
guest reads still return EFAULT. Existing restore writes/reprotects are reused.

Using CC=/usr/bin/gcc and CXX=/usr/bin/g++:
- `make NP=1 -j4 check-mappings check-state-io check-page-snapshot
  check-thread-signal-mask check-thread-tid check-thread-lifecycle check-exec
  check-scenarios` passes: six mapping variants, eight mask variants, eight TID
  variants, lifecycle coverage and fourteen tracer scenarios.
- `make NP=2 -j4 check-syscall-policy check-capture check-runtime-profile
  check-process-state check-io-boundary check-redis` passes.
- `make check` passes nine component and ten build-contract tests.
- Production NP1/NP2/NP3 builds pass. GCC strict syntax checks pass for capture
  source and the mapping fixture; combined dispatcher check disables only its
  existing unused-parameter warnings. Independent review and diff check pass.
- Raft depth8 single-run full/pages both produce 2,582 visits/1,347 new states,
  about361/549 states/s. Artifacts build/guard-raft{,-pages}; not a statistical
  performance claim. Large unreadable reservations still require full reads and
  restore writes.
- The actual pthread TLS/join probe now reaches the active-rseq clone rejection;
  build/tid-pthread-probe/guard-transcript.log records it.

An intermittent identity failure in the old mapping fixture was traced to exactly
rseq cpu_id_start/cpu_id at FS_BASE+__rseq_offset. The fixture now unregisters the
inherited registration, matching existing raw-memory fixtures. No product rseq
normalization or thread-runtime-specific patch was added.

## rseq-unavailable verification (2026-10-16)

The chosen fixed syscall-unavailable environment is reflected in kernel modelv8
and RuntimeProfile identity. Deletes the old globals, 0x55 normalization and live
byte restoration instead of adding a runtime-specific state extractor.

Using CC=/usr/bin/gcc and CXX=/usr/bin/g++:
- RED: lifecycle native registration allowed snapshot publication on HEAD;
  `/tmp/rseq-red.log`. GREEN: four full/pages x normal/tiny lifecycle variants,
  including main/worker/spare capture, same/different state restore and all three
  directly callable carrier/memory restore entry points rejecting without mutation.
- NP1 thread-control/carrier/identity/TID/signal-mask/futex/futex-snapshot/exec/
  state-io/mappings/page-snapshot gates pass, plus 14 native tracer scenarios.
- NP2 runtime-profile/syscall-policy/process-state/capture/io-boundary/Redis pass.
  Valid, invalid and unregister rseq arguments return ENOSYS; user buffers and
  actual kernel registration are unchanged.
- `make check`: nine components and ten build-contract tests pass. Strict GCC14
  syntax checks pass on changed capture/control/lifecycle sources and startup/
  policy fixtures; combined dispatcher check suppresses only its existing unused
  parameter warnings. Independent read-only review and diff check pass.
- NP1/NP2/NP3 production builds pass. Raft depth8 full/pages both visit2,582 states
  with1,347new; single-run throughput387/516 states/s. Artifacts
  build/rseq-unavailable-raft{,-pages}; no statistical performance claim.
- Native seccomp filter returning ENOSYS for rseq from exec permits the unmodified
  pthread probe to exit0 (build/rseq-contract). DetSim advances to unsupported
  syscall273/set_robust_list (build/tid-pthread-probe/rseq-unavailable-transcript.log).

Full rseq critical-section semantics are explicitly outside this environment.
Auxv size/alignment hints are retained; the model is syscall-unavailable, not a
claim to reproduce every property of CONFIG_RSEQ=n. Full pthread support remains
blocked by additional generic Linux task/futex/signal lifecycle work.

## Pthread lifecycle delivery (2026-10-16)

Changes span thread_pool/control/state.h (logical robust registration and non-PI
owner-death), dispatcher/handlers/fsstate (anonymous discard and archived virtual
file mapping provenance), state_transition (startup ABI and owned-thread cleanup),
StateView, and focused native regressions. Reuses ordinary memory snapshots,
FutexState shared/private queues and existing mapping restore. Removes the old
blanket nonempty robust-list rejection and leader-only termination wait; no
pthread-private layout patches or new dependency are introduced.

Reproduced failures before the fixes:
- set_robust_list rejected as syscall273; pthread exit later rejected madvise28.
- Virtual file mmap appeared anonymous in /proc and wrongly admitted discard and
  guard conversion; brk shrink also left stale provenance.
- The real pthread workload reached exit0 but tracer q hung. strace showed kill
  followed by leader-only wait while a retained ptraced worker remained unreaped.

Verification with CC=/usr/bin/gcc and CXX=/usr/bin/g++:
- `make check`: nine component and ten build-contract tests pass.
- `make NP=1 -j4 check-thread-robust check-thread-lifecycle check-thread-control
  check-thread-carrier check-thread-identity check-thread-tid
  check-thread-signal-mask check-futex check-futex-snapshot check-exec
  check-state-io check-mappings check-page-snapshot check-scenarios` passes.
  Robust4 backend/cache variants include eight native owner-death oracles,
  pending-zero wake, PI preflight, group ordering/aliasing and bootstrap adoption.
- `make NP=2 -j4 check-runtime-profile check-syscall-policy check-process-state
  check-capture check-state-view check-plugins check-io-boundary check-accept
  check-redis` passes. Provenance is checked in real mmap/munmap/brk operations,
  branch copy/archive and full-process persisted restore plus discard rejection.
- Fifteen native scenarios pass. The new libc pthread workload checks three
  create/join generations, TLS isolation, robust EOWNERDEAD recovery and mutex/
  condition wait/signal. Full/pages x normal/SIGTERM cleanup all end correctly
  with every retained physical TID gone. Unrelated-sibling gates still pass.
- Startup group/session escape regression passes; the final scenario gate was
  rerun after this fix. Strict GCC syntax checks and git diff --check pass;
  dispatcher syntax suppresses only pre-existing unused-parameter warnings.
  Independent read-only design and final review found no remaining blockers.
- NP1/NP2/NP3 production builds pass. Raft depth8 full/pages both visit2,582 states
  with1,347new; single-run throughput372/521 states/s. Artifacts
  build/pthread-lifecycle-raft{,-pages}. These samples do not prove performance
  equivalence. The standalone pthread/strace artifacts are in build/pthread-lifecycle.

Flat21/page22 and kernel modelv9 carry the new state contract. Remaining limits:
PI/timed futexes, cancellation, asynchronous delivery, full rseq semantics,
mutable file coherence, broader madvise behavior, process trees and exhaustive
thread/shared-memory interleavings. Successful pthread scenarios certify their
controlled paths, not unrestricted pthread or Go runtime support.

## Generic pending syscall verification (2026-10-16)

Product changes: engine/pending_syscall owns Waiting/Retry/Ready, immutable call
arguments, captured normalized deadlines and explicit completion; pending_io
adapts existing resource emulators and descriptor lifetime guards. Scheduler and
snapshot paths derive blocking from the saved pending state. Futex's duplicate
timeout flag and CLI's ad-hoc blocked rollback are removed. Event observation is
ABI2 (including nonreturning crash/exit phases), with matching example plugins.
Normal supported process exit closes descriptors so peer reads can complete EOF.
No new dependency or target-runtime-private extraction was introduced.

RED: a real futex WAIT exposed return0 before wake/completion. The initial Redis
gate additionally demonstrated that finite epoll waits need captured pending
state rather than a fabricated timeout or unconditional unsupported path.

With CC=/usr/bin/gcc and CXX=/usr/bin/g++:
- `make check` passes nine component and ten build-contract tests.
- NP1 `check-pending-syscall` passes full/pages x normal/tiny: real ptrace WAIT,
  frozen arguments/deadline, wake vs explicit expiry, Ready and completion disk
  replay, no user instruction on completion, real sleep registration and replay.
- NP1 futex13, futex-snapshot, thread TID8, robust4, lifecycle4, signal-mask8,
  control/carrier/identity, exec, state-io, mappings and page-snapshot gates pass.
  Old fixtures now represent coherent waiting syscalls instead of isolated queues.
- NP2 `check-pending-io` covers blocking/nonblocking read/write, capacity/atomic
  pipe writes, accept4 flags, TCP/UDP and epoll readiness, copied17ns pwait2
  deadlines, descriptor mutation rejection and explicit expiry. Blocking writes
  requiring partial in-flight progress reject before mutation.
- NP2 `check-pending-io-snapshot` passes four backend/cache variants: whole-system
  waiting/retry/completion persistence, cross-node TCP delivery, no duplicate
  consumption, competing driver read, output-page EFAULT, sender final-thread/
  exit_group EOF and repeated before/after-exit branch replay.
- NP2 capture/identity/state-view/plugins/runtime-profile/syscall-policy/
  process-state/io-boundary/accept/Redis gates pass. Additional malformed snapshot
  test rejects pending IO with no retained descriptor. ABI1 is explicitly rejected.
- Seventeen native tracer scenarios pass, including a phase-checking ABI2 plugin
  over real pthread/pipe blocking and a fatal instruction stop. Pending CLI output
  never displays the internal placeholder as a completed return.
- Strict GCC diagnostics on changed core/IO and native fixtures pass; unused
  parameter warnings from old handlers are excluded in the combined invocation.
  Independent architectural/final review and git diff --check pass.
- NP1/NP2/NP3 production builds pass. Raft depth8 full/pages both visit1,511 states
  with782new; single-run throughput366/486 states/s. Artifacts
  build/pending-syscall-raft{,-pages}. Prior semantics visited2,582/1,347: this is a
  changed transition system, so reduced elapsed time is not an optimization claim.
  Equal counts do not establish exhaustive exploration equivalence.

Fresh verification logs: /tmp/detsim-pending-validated-np{1,2}.log,
/tmp/detsim-pending-release-np{1,2}.log, /tmp/detsim-pending-release-check.log,
/tmp/detsim-pending-release-scenarios.log, /tmp/detsim-pending-validated-syntax.log.
Source timestamps were refreshed for a final controlled rebuild after agent edits
so linked verification does not depend on objects compiled during source changes.

Scope: no automatic time advancement or expiry/actor/outcome choice policy.
Pending descriptors cannot be concurrently closed/rebound; pinned resource
identity is future work. Pending UDP retry currently chooses the first datagram.
TCP send queues remain unbounded; unread-data reset, unaccepted-server-exit reset,
fatal-crash teardown, partial-progress blocking writes, poll/select/vector IO,
signals/cancellation and independent realtime/monotonic axes remain scoped gaps.
Snapshot versions23/24 and kernel modelv10 encode the new semantic state; plugins
must rebuild for detsim_plugin_entry_v2 and honor event phase/result_valid.

## Case-driven acceptance (2026-10-16)

Added examples/cases: independent native oracles, public-ABI property plugins,
healthy/mutant input and pthread workloads, six installed-runtime probes, and a
bounded runner retaining commands, profiles, hashes, transcripts and witnesses.
Each search owns a fresh tracer/store. Explicit unsupported, bounded and missing
compiler outcomes cannot become successful admission. No new dependencies or
workload-specific product extraction were added.

Cases drove three product fixes: dynamically sized argv (removes the five-slot
limit and terminating-null overflow), ordinary anonymous zero-length mmap EINVAL
without bypassing file-descriptor precedence or unsupported huge pages, and
nonblocking poll over the existing descriptor/readiness models. Native oracles
also caught epoll POLLRDNORM and partial cross-page revents writes on EFAULT.
Blocking poll is still unsupported. Kernel modelv11; snapshot layouts23/24.

Final verification with CC=/usr/bin/gcc CXX=/usr/bin/g++:
- make NP=1 -j4 check-cases check-scenarios: 12 healthy/mutant/backend rows pass;
  both bug classes found and search witnesses replayed three times; 18 native
  scenarios pass. Runner regressions reject depth exhaustion and absent tools.
- make NP=2 -j4 check-syscall-policy check-io-boundary: pass, including mmap
  precedence/huge-page and native poll cross-page atomic scalar fault oracles.
- make check: nine component cases and ten build-contract tests pass.
- Production NP1 tracer builds; Python compile checks and git diff --check pass.
- Runtime audit using the final NP1 binary and both backends: all six native
  probes complete and all six controlled-step/restore probes pass. C/C++ checked
  model execution completes; Rust stops at syscall131, gccgo at204, Python/JVM
  at89. Audit correctly returns1. This is not full runtime admission.

Final artifacts: build/cases/run-a6_xdkg5/results.json (cases),
build/cases/run-q96si028/results.json (runtime audit). Logs:
/tmp/detsim-cases-final{,-boundary,-check,-runtimes}.log.

Remaining: standard gc Go, all four runtime blockers and their later successors,
real distributed protocol mutations, complete actor/outcome enumeration and
shared-memory atom boundaries. Same-process replay is not cross-ASLR portable
replay; the pthread case establishes a concrete closed-world deadlock, not full
interleaving coverage. Existing DFS completion text/visited reuse remains debt.

## Runtime path/CPU admission and compressed capacity (2026-10-16)

Changes: captured VFS symlink targets, scoped readlink/readlinkat and lstat,
startup executable path/content capture, and fixed CPU0 sched_getaffinity using
logical live task IDs. Existing VFS content/type serialization is reused; no new
state fields or runtime-specific framework branches. Kernel modelv12, byte
layouts23/24. Missing/non-directory prefixes, executable conflicts/ETXTBSY and
unmodeled symlink/proc operations reject rather than manufacture success.

Regression evidence with /usr/bin/gcc and /usr/bin/g++:
- RED actual runtime-query scenario stopped at unsupported204; final native
  scenario restores and replays affinity/readlink/readlinkat edges three times
  each in full/pages. NP1 check-scenarios:19 tests; check-cases:12 rows plus
  bounded/missing-toolchain runner checks.
- NP2 check-process-state, check-syscall-policy, check-runtime-profile and
  check-io-boundary pass. Native readlink/error/ABI oracles, archive branch
  isolation, deletion of host link after capture, and affinity identity/error
  oracles pass. Filesystem and process changed files pass strict C++ diagnostics.
- NP2 check-redis passes SET/GET, client exit0;26.012s versus earlier7.691s.
  This is a measured regression from importing a16MiB executable into an owned
  VFS vector copied with every state. Small12-case run12.43s before/12.19s after
  does not negate the large-target cost. Immutable VFS content needs a follow-up.
- make check passes9 component cases and10 build contracts; NP1/NP2 production
  builds pass. Independent read-only semantic review and diffcheck pass.

The runtime audit additionally found compressed vector capacity retained at raw
snapshot scale while cache accounting charged only compressed length. A separate
focused fix constructs compact encoded storage; an8MiB compressible regression
failed before and passes afterward with exact decompressed bytes. NP2
check-store-packed and check-store-cache pass, strict C++ diagnostics pass.
Save-side decompression validation remains unchanged. GDB localized a separate
remaining full-mode transient747,734,229-byte allocation failure in decompress
under4GiB; the compact-buffer fix is not a global memory budget guarantee.

Logs: /tmp/detsim-v12-{release,boundaries,redis,final-cases,final-check}.log,
/tmp/readlink-io-final.log, /tmp/detsim-compress-capacity-{red,green}.log.
Runtime final artifacts: build/cases/run-z65kj5m2/results.json; Python controlled
steps: build/cases/python-steps-lkpxv54e/session; full allocation diagnostic:
build/cases/go-affinity-gdb-size-3uhecnub/gdb.log.
Current failed probes: Rust/gccgo-pages sigaltstack131, gccgo-full allocation,
Python missing standard-library mapping and exit1, Java access21. All retain
non-green status. Generic symlink traversal, complete procfs, CPU-input coherence,
standard gc Go and exhaustive scheduling remain outside this slice.

## VFS payload optimization plan (2026-10-16)

Invariant: snapshots own a semantic byte sequence, never a mutable buffer alias.
Remove repeated copying and inline serialization of large unchanged VFS contents;
retain existing file behavior and exact collision checks. First retain existing
filesystem regression coverage and record a same-binary Redis baseline. Replace
owned vectors with shared private payloads and completed write/assign operations;
never expose mutable pointers. Persist large payloads through existing StateStore
objects, with explicit tagged reference encoding only in production capture.
Standalone archives remain self-contained. Registration memo is transient and
store-generation scoped; cached hits still validate store health. Missing,
corrupt and mismatched references fail, without live-host fallback.

Acceptance: shared copies and branch writes/truncation isolate; inline archives
work with no store; reference bytes are independent of registration/cache order;
repeat saves avoid re-registering unchanged content; zero-cache/read-only reopen,
new store generation and poisoned store behave correctly. Actual full/pages
restore must recover file bytes across writes. Run existing IO, snapshot, plugin,
storage and native gates, then compare repeated Redis scenarios and case outcomes.
Snapshot layouts bump to25/26 for the new wire representation; no guest semantic
change or new dependency. Immutable payload ownership remains scoped to reachable
states/readers; no global unbounded interning table.

VFS plan completion evidence:
- Original HEAD1MiB node-copy sharing assertion RED; same pointer sharing plus
  explicit write/clear isolation GREEN. No mutable content pointer is exposed.
- check-vfs-content passes all new reference/identity/generation/corruption/poison
  oracles, including a fresh exec reader with zero caches. Default make target
  selectsNP1; NP2 explicit run also passes.
- NP1: check-state-io, check-page-snapshot, check-mappings, check-storage and
  check-integration pass. Storage-tools fixture was stale DSS4 rather than
  production DSS5/v2; updated valid fixture and added explicit legacy rejection.
- NP2: check-io-boundary, check-state-view, check-plugins, check-capture,
  check-identity, check-pending-io-snapshot pass (full/pages/cache variants).
-20 native scenarios and12 healthy/mutant case rows pass; make check9 component
  cases/10build contracts pass. Changed C++ files/new integration fixture pass
  strict compiler diagnostics; Python compile/diffcheck pass; independent
  architect review approves. Production NP1/NP2 builds pass.
- Redis comparison records12 successful fixed-schedule runs in
  build/vfs-comparison/results.json,3each before/after × full/pages. Identical
  server208/client58 steps. Full before[27.114,26.592,26.845]s after[7.553,7.318,
  7.659]s; pages before[19.514,19.281,19.506]s after[2.024,1.980,1.891]s.
  Median wall speedups3.55x/9.85x; GNU-time maxRSS medians471.9→319.0MiB/full,
  510.9→168.1MiB/pages; stored memory+sstate bytes2158.6→599.7MiB/full,
  1577.6→17.3MiB/pages. This is not an exhaustive-search performance guarantee.

Logs: /tmp/detsim-vfs-{reference-final,gates,native,cases,snapshot-gates,
system-gates,final-gates,check,comparison}.log. Cases artifact
build/cases/run-uj0r661q/results.json. Format25/26; unchanged kernel modelv12.

## Runtime admission iteration2 evidence (2026-10-16)

Changes: per-task virtual alternate-stack registration/query and bootstrap flags;
correct FSMap root file/directory/symlink import; F_OK access family; explicit
Python/JDK workload input manifests. No private interpreter/JVM state extraction.
Kernelv13 fixed altstack minimum2048; snapshot layouts27/28.

- RED real sigaltstack transition unsupported131; native oracle covers accepted
  ONSTACK normalization, AUTO, min size, alias, EPERM/errors and partial copyout.
  check-thread-altstack full/pages×default/tiny passes persisted replay, logical
  clone/exit/carrier reuse and physical-disabled invariant. Five malformed states
  fail capture and serialized restore without publication/live-state drift.
- Review caught disabled AUTODISARM bootstrap loss; RED/green native regression
  now preserves it. Enabled inherited stacks remain explicit unsupported.
- FS root-file regression RED; root/ancestor overlays separately RED→GREEN.
  NP2 check-io-boundary/check-syscall-policy/check-process-state/check-runtime-profile
  pass. Native F_OK ABI, exact import, failures, host-unlink/archive and branch
  isolation covered. Permission modes needing credentials remain unsupported.
- NP1 signal-mask/lifecycle/altstack gates pass;21 native scenarios including real
  access/faccessat/faccessat2 (old faccessat ignores arg4) pass;12 case rows and
  runner negative gates pass; make check9 components/10build contracts pass.
  Changed core/tests compile warning-clean; independent review approved after
  the bootstrap and ancestor fixes. Production NP1 build passes.
- Runtime audit depth1000/90s still non-green: Rust full/pages pass, gccgo pages
  reaches270 and full remains allocation-limited; Python rootfs reveals retained
  DFS ancestor content copies; Java reaches jvm.cfg symlink traversal. Evidence
  build/cases/run-2j44plxy/results.json, /tmp/detsim-runtime-iteration2-audit.log.
  Not all languages are admitted yet. User permits conditional Java tradeoff.

Logs: /tmp/detsim-{altstack-red,iteration2-thread-final,iteration2-fs-final,
iteration2-cases-final,iteration2-check}.log. Python allocation census:
build/cases/python-memory-debug-qtd8mky7/gdb.log. Rootfs bytes59262067/1408 entries
are fixed inputs; ~30 duplicate large buffers prove live-ownership growth. Next
slice releases DFS ancestor resident state while preserving hashes/search order.

## DFS ancestor residency (2026-10-16)

Plan before edits: retain ancestor hash/search bookkeeping, release fully loaded
payload ownership on descent, reload only for actual backtracking; preserve
choice/state1/2/3 ordering and avoid new global interning. RED on actual CLI with
64MiB VFS and2GiB cap; old small DFS and large BFS pass, old large DFS fails in
both backends. GDB allocation census showed30 identical large-file buffers.

Implementation: optional DFSFrame resident state, reset after successor capture
and parent bookkeeping, lazy reconstruction in states1/2. Generic deserialize
error includes original cause. Historical recursive DFS is unchanged and not the
CLI execution path. No snapshot format or guest semantics change.

GREEN: tests/dfs_residency.py against immutable production NP1 binary, eight
full/pages×small/large×BFS/DFS searches; every graph81unique/80actualtransition
multiset/fourbranches. Mutation and terminal predecessor each3x exacthash replay.
Artifact build/dfs-residency-green/report.json; RED build/dfs-residency-red-full
and build/dfs-residency-red2. Python full/pages now reach217 with same2GiB cap:
build/cases/run-r0hsdkcb/results.json. make check passes, Python compile/diffcheck
pass, production NP1 build passes; independent architect review approves. Scope
is this finite NP1 equivalence and replay, not arbitrary scheduling completeness.

## Runtime iteration4: pselect and symlinks (2026-10-16)

Zero-FD pselect timer reuses pending syscall state (no layout fields); positive
wait/deadline, NULL infinite wait, explicit completion, native timeout-copy/error
semantics and scratch protection. Captured-only symlink walker adds40-link and
NAME_MAX bounds, canonical open/fstat, nofollow/O_EXCL and trailing creation rules.

RED old syscall270 rejection and old symlink traversal rejection; review found
scratch output risk and long-name behavior, each fixed with RED→GREEN regression.
Root NP1 pending4variants,21native scenarios,12cases+runnernegative gates pass;
NP2 IO/syscall policy pass; compiler syntax/warnings and diffcheck pass. Independent
review approves both corrected slices. make check passes. Kernelv14, layout27/28.
Runner's successful endpoint now replayed3x, with completion artifact; explicit
memorylimit flag is reported rather than hidden budget changes.

Audit build/cases/run-6m_rj7fv/results.json: Rust bothbackends complete+3xreplay;
gccgo full4GiB/90s times out in progress, pages exposes fatal nanotimezero then
blockedthreads; Java bothbackendsexit6 after libjvm dlopen (fileidentityhypothesis).
No newruntimegreen exceptRust. Next generic fixes: initialvirtualclock profile,
consistent file inode identity/getdents64, then actual next blockers. Logs:
/tmp/detsim-iteration4-{final-np1,final-np2,runtime-audit,runner,check}.log.
