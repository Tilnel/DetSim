# Correctness backlog

These are source-supported findings, not all reproduced end to end. Component/build
checks do not close these items. Priority reflects risk to exploration soundness.
Use one item per focused change, with a failing regression before the fix.
The [state/atom contract assessment](model-checking-contract.md) explains their
relationship to the native-binary model-checking goal.

| ID | Priority | Evidence / problem | Evidence required to close |
| --- | --- | --- | --- |
| C01 | Fixed (unchanged live thread set) | Futex queues are branch-owned, captured and archived; restore reconstructs blocked state, including same-hash restores. | `check-futex-snapshot` exercises capture/copy/assignment/archive, persisted reload and repeated ptrace restore/wake. Raw lifecycle/carrier restore has separate C02 tests; combined lifecycle and futex waiting still needs wider coverage. |
| C02 | P0 / partial | Capture preserves virtual slots and creation records; raw clone/exit now restore logical contexts over retained carriers; unexpected physical death or untracked creation rejects. Physical TIDs now stay in transient bindings; explicit liveness and logical parent IDs enter archives. Guest-visible physical IDs in register/memory contents are not normalized. | Create A/B, exit A, create C, repeatedly restore with correct logical identity and TLS/stop phases. `check-thread-lifecycle` now covers that sequence, TLS, persisted replay, main-first exit and actual carrier reuse. pthread, TID writeback, robust owner-death and rseq remain unsupported. |
| C03 | P0 | New BFS sets a selected thread before `state_transition`; recovery and `execute_step` synchronization can overwrite that selection. Two exploration implementations remain compiled; CLI uses the old one. | A two-thread finite program exposes both successor transitions from one snapshot. Trace all callers, compare BFS/DFS results, then consolidate to one path. |
| C04 | Partial | Ordered system metadata identity includes status/budget; storage now compares duplicate bytes in cache, packed records and metadata instead of trusting hashes alone. | Identity and all storage gates cover ordering and duplicate conflicts. Logical thread identity and equivalence of differently encoded semantic states remain open. |
| C05 | Fixed (scoped) | Removed filename heuristics; corrected libelf string-table index in Go-section detection. | Native `test_probe` starts at the same boundary as `probe`; ordinary/Go-section ELF fixtures and two existing gc Go binaries classified correctly. gccgo and actual Go runtime execution remain unverified. |
| C06 | Fixed (framework boundary) | Removed Raft fields, implicit role pruning, message inference and example parser from the production link. Versioned plugins inspect explicit snapshots; branch-owned opaque monitors enter product identity before successor publication. | `check-state-view`, `check-plugins`, `check-identity` and native counter-plugin restore/violation scenarios cover readonly inspection, ABI failures, history isolation, terminal witnesses and decoder noninterference. Raft examples have separate projection/history contracts; this does not certify every Raft property. |
| C07 | Fixed | Snapshot reads now require saved bytes and reject missing/corrupt data; no live-memory substitution. | `check-state-io` covers missing/invalid archives and reads spanning saved regions. |
| C08 | Partial | Native same-edge replay preserves hash; storage tests now cover immutable reader lifetime, cache pressure/zero budgets, short I/O, prefetch and fresh-process reload. | `check-storage`, `check-state-io`, `check-capture` full-byte oracle, mapping and native replay gates pass. Full exploration equivalence across cache policies and general multi-node semantic canonicalization remain unproven. Virtual-exit/fatal-stop branch closure now has real ptrace regressions: persisted terminated images load and restore along with live peers; recapture preserves identity. Physical process death still rejects; lifecycle reconstruction remains C02. |
| C09 | Fixed (scoped) | Replaced process-group termination with owned-child kill/reap; signal handler avoids store shutdown locks. | NP=1 scenarios verify unrelated sibling survives both normal exit and SIGTERM, and SIGTERM reaps tracees. Fork descendants, multithread races and initialization failures need additional coverage. |
| C10 | Fixed | Migrated removed libdwarf calls to supported APIs; validated with installed libdwarf 0.11.1 and GCC 14.2. | Real production parser tests pass DWARF 4/5, two CUs, sizes/member offsets, symbols, arrays and cleanup/reinit; tracer compiles and links. Split DWARF and PIE relocation remain outside coverage. |
| C11 | Fixed (distinct addresses) | Direct `futex_requeue` now updates `waiter_map_` when moving a waiter. | Both entry points agree on queue/address; archive round trip followed by `wake_thread` succeeds. Same-address and syscall requeue ABI now have C16 regressions. |
| C12 | Fixed | `handle_exit_group` wrote a raw wait status without the DetSim exited flag; successful exit became “running” and the next step crashed. | Finite program reaches counter 1/2/3, exits virtually, and further stepping reports exit status 0. |
| C13 | Fixed (BFS/DFS/RAND) | BFS formerly continued after violation; DFS ignored its stop code and RAND only broke an inner loop. All now stop the entire search, retaining a failed product-state witness. | Native scenarios verify reachable assertion violations stop each mode without a false completion; counter-plugin replay preserves terminal identity. Ordinary finite BFS still explores the expected five states. |
| C14 | Fixed | Batch command parser pointed at the NUL inserted by strtok and discarded all arguments. | Real batch handles whitespace, `sw 0`, `p counter` and `q`. |
| C15 | Fixed (captured streams) | STDIO descriptors share the virtual FD registry; stdin is deterministic EOF and stdout/stderr content is archived VFS state. | `check-io-boundary` covers access modes, writes, close, archive and branch isolation; terminal input/output and interactive stdin remain unsupported. |
| C16 | Fixed (scoped) | WAKE unblocks removed waiters, successful WAIT saves a suspended successor with return 0; requeue uses arg4 count/arg6 comparison, returns total affected and handles same address safely. | `check-futex` covers real ptrace WAIT/WAKE/bitset/requeue/errors; unsupported timed/realtime/PI operations explicitly fail. Exhaustive wake-order outcomes remain outside coverage. |
| C17 | P1 | The glibc-private CPU-feature patch has been removed; general nondeterministic CPU instructions/runtime inputs still need a supported execution contract. | Define and test which nondeterministic CPU/runtime state must be normalized; avoid assuming this patch worked from a successful scenario. |
| C20 | Fixed (host user XSTATE) | Capture/archive/restore NT_X86_XSTATE for each thread; snapshot version 5 rejects old formats. | `check-xstate` covers XMM/AVX/x87/MXCSR; native SIMD scenario repeatedly restores observed=0 and replays observed=11. Cross-host layouts and supervisor state are outside coverage. |
| C21 | Fixed (scoped) | Save private readable mappings including RO pages; restore protections/backing and canonicalize equivalent VMA partitions. Reject shared/unreadable maps and missing file/named backing. | `check-mappings` covers RO contents, permissions, split/merge, anonymous recreation, explicit unsupported cases and wide map fields. Native replay preserves successor identity. |
| C22 | Fixed (snapshot I/O) | Exact read/write counts and archive bounds required; failures propagate, without zero-fill or live fallback. | `check-state-io` fault-injects short/failed process_vm I/O and corrupt/missing/old archives. Failed restore is fatal; no successor may execute afterward. |
| C23 | P0 / partial | TCP source ports now derive from saved socket state, independent of search RNG; clocks/getrandom now use saved virtual state; custom signal delivery, masks and asynchronous kernel inputs remain unsupported. | `check-port` covers replay, RNG independence, source addresses, uniqueness and exhaustion. Closed syscall policy and process-model regressions cover rejection, deterministic replay and archives; this does not close general asynchronous inputs. |
| C24 | Fixed | Populate current syscall number/arguments before the entry handler. | `check-exec` exercises real ptrace FUTEX_WAKE suppression using current arguments. |
| C25 | P0 / partial | Auto execution now returns after each completed syscall, including CKPT_NO. Runnable threads/outcomes are still not exhaustively enumerated; pure user loops and blocking kernel calls are not bounded. | `check-exec` verifies empty receive, sleep and getpid loops return control. Futex waiting successors are now preserved; enabled actor enumeration and shared-memory atom boundaries remain open. |
| C26 | Fixed | Pending accounting covers queued and executing work; completion waits and synchronizes packed data/index. Async and sync errors poison subsequent operations. | `check-store-cache` holds a worker at compression, proves barrier stays blocked, then opens a fresh read-only process while writer remains alive. Sync failure and shutdown/reinit also covered. |

| C27 | Fixed | Non-owning shared cache buffers and eviction mutations caused wrong bytes and crashes under pressure. Immutable owned buffers replace mutable cross-thread entries; cache and pending budgets are explicit. | `check-store-cache` covers pointer lifetime, zero/tiny caches, concurrent dedup, multiworker and prefetch. Fixed-data constrained-cache benchmark passes every byte comparison. |
| C28 | Fixed (validated local storage) | Packed raw checksum mismatch, unsafe index/journal publication and destructive maintenance; metadata index merge could lose records and accepted conflicting hashes. | `check-store-packed`, `check-store-metadata`, `check-store-tools` cover short I/O, faults, recovery/append/barrier/reopen, corruption and non-destructive maintenance. Partial files fail explicitly; arbitrary power-loss recovery is not established. |

## Next development slice

Page snapshot branch (2026-10-15): opt-in page manifests (now v12) and page objects use the
existing barrier; flat snapshots (now v11) stay default. Exact decoded flat/page ptrace oracles, cold/tiny
reads, fresh-process inspection, missing/corrupt pages and warmed-cache poisoning
have regressions. Both formats compare live bytes and restore only differing pages.
This strengthens C08 evidence without closing general canonicalization, complete
exploration equivalence or asynchronous dirty tracking. See [page snapshot scope
and whole-chain timings](page-snapshots.md).

Soft-dirty experiment (2026-10-15): opt-in verify reconstructed all11,127 Raft
captures without observed omitted pages; native fixtures also reject a deliberately
lost dirty epoch. This does not close C08 or certify asynchronous/pinned writes:
the baseline-read/clear_refs window, epoch exclusivity and PID/exec lifetime need
a supported contract. The full backend remains default; end-to-end timings show
no gain in the measured workload. See [measurement and limits](redisraft-benchmark.md).

Redis smoke evidence (2026-10-14): [redis-smoke.md](redis-smoke.md).
Redis findings and resolution:

- **C18 / fixed:** Indexed type definitions once, removed nested CU scans, and
  guarded recursive aliases. Forward/opaque same-name typedef and C++ class4/5
  regressions pass; original Redis debug initialization takes about 0.34 seconds
  and 64 MiB RSS under a 1-GiB cap (previously >45 seconds / 6.2 GiB sampled RSS).
- **C19 / fixed for the supported path:** Checked guest sockaddr/length copies,
  length writeback, truncation, fd/peer matching and stale queue handling pass 12
  cross-process cases; original Redis SET/GET now completes. Pending connections now use captured endpoint keys; same numeric client FDs
  across nodes no longer alias. Snapshot formats are now11/12 (I/O repair first introduced7/8).
  EFAULT retains the virtual pending connection; partial copyout is not rolled back.
  These error-path modeling limits and accept4 flag fidelity need further coverage.

The first state/atom repair pass adds XSTATE, strict snapshot I/O, scoped mapping
restoration, ordered system identity and a completed-syscall reference boundary.
Next define and enforce the supported binary/environment/property profile, close
remaining host inputs (C23), and complete thread lifecycle restoration C02 and explicit
actor enumeration C03/C25. C01/C16 now have scoped branch-state/syscall regressions. Consolidate schedulers only with successor-set
and property-observation comparisons. Eviction and wider
mapping coverage (C08) remain necessary for broader workloads.

Current scenario evidence is NP=1, native non-PIE, single-thread, with inherited
ASLR unchanged. It verifies repeated restore within one live process, not cross-run
canonical hashes, multi-node symmetry, complete syscall fidelity or general search
soundness. The early single-thread commit (`c21c496`) remains a design reference,
not a proven oracle.

## Joint virtual I/O boundary slice (2026-10-15)

A shared typed descriptor registry now owns allocation and flags. Files, pipes,
sockets and epoll dispatch through one read/write/close boundary. Guest copies
are exact; failed copyout retains offsets/queued payloads (guest memory may have
a partial prefix written). Archives include pipe endpoints/queues, pre-accept
TCP payloads, socket options and epoll opaque userdata. Old format5/6 is rejected.
Unsupported descriptor/path syscalls stop before host kernel effects; this is a
supported profile, not complete Linux emulation.

Still open: blocking read/accept/epoll wakeups/timeouts; dup/shared open-file
descriptions; cross-node shared filesystem ownership; symlinks; advanced epoll
ET/ONESHOT/nesting and temporary signal masks; TCP timers/Nagle/keepalive effects.
Private VFS mmap copies initial bytes and restores permissions, but does not track
subsequent file writes into clean mapped pages. fsync validates a virtual regular
file without modeling filesystem crashes/durability. These require new transition
and snapshot contracts, rather than success-only syscall stubs.

## Generic framework slice (2026-10-15)

Machine snapshots no longer contain application/checker fields. Flat9/page10 share
SnapshotView chunk readers; opaque per-plugin history is persisted in version2
system metadata with model context and terminal failure identity. Model fingerprints
cover executables, semantic plugin binaries/configuration and runtime profile,
excluding caches, UI and decoder-only plugins. Incompatible states fail explicitly.

Known support limits: trusted native plugin ABI, fixed NP in core, symbolic
inspection of a matching non-PIE main image only, immutable executable files during
a run, explicit Linux syscall bootstrap outside exploration, and time-choice-only
external environment callbacks. Generic network/futex outcomes remain in platform
models. Signal delivery and universal CPU nondeterminism are not solved by removing
workload patches. C02/C03/C08/C23/C25 remain open at their stated scope.

## Closed exploration syscall profile (2026-10-15)

Unknown calls now reject before kernel execution; the default route no longer
passes through. The explicit native allowlist retains owned-process identity and
credential queries, query-only own resource limits and supported private memory
operations. Mapping operations reject scratch-page overlap and range overflow.
Clone/clone3 and thread exit reject until lifecycle restoration is implemented.
The execution entry rejects i386/x32 ABIs and noncanonical wide syscall numbers.

Flat11/page12 include deterministic RNG state and signal dispositions; filesystem
umask is saved with VFS state. Clocks and sysinfo read a fixed virtual environment.
Startup imports actual dispositions/umask and rejects preexisting multiple threads,
blocked/pending signals, active interval timers and alternate stacks. Nonzero
sleep, armed timers and custom signal delivery fail explicitly. Trusted startup
and internal restore injections remain separate from modeled exploration.

`check-syscall-policy` uses native ptrace to prove PR_SET_NAME cannot escape to
host state, validates allowed queries/errors and guards reserved memory.
`check-process-state` covers copy/archive, RNG replay and EFAULT rollback, signal
policy and actual startup inheritance. `check-capture` perturbs RNG/actions to
prove identity changes and repeated restore for flat/page snapshots. These tests
close the default unknown-call escape; C02/C03/C08/C23/C25 remain scoped/open.
General signal delivery and exhaustive random outcomes are not implemented.

## Parked terminal images (2026-10-15)

Virtual exit and intercepted fatal signals leave an owned physical task parked.
Loading and restoring their product states must include those images: skipping
terminated-node metadata or memory mixes a different branch into later captures.
The restore path now loads every node and restores every differing image. Even
same-hash restores validate the owned stopped physical thread set; a genuinely
dead task rejects instead of reporting success. The transition entry rejects an
invalid or terminated actor before selecting/restoring it. The kernel-model
compatibility marker advances to v2; snapshot byte layouts stay flat11/page12.

`check-capture` reproduces the old virtual-exit memory leak, then covers a real
`exit_group` and `ud2`/SIGILL, alternate memory/RNG/umask/time branches, persisted
reload, exact recapture identity, predecessor restore and actual OS death. Both
normal and tiny caches run against flat and verify-pages backends. This closes
that C08 branch-contamination case; changed physical thread sets, stop-all
coordination and general canonicalization remain open.

Virtual termination still retains modeled descriptors; Linux exit-driven close/EOF
and other resource teardown semantics require a separate lifecycle model. The
regressions above establish branch-image isolation, not complete exit semantics.

## Raw thread carrier lifecycle

Logical clone/exit reconstructs thread contexts across births and exits using
retained stopped carriers; the physical group leader remains an anchor. Pool
spares do not enter snapshots. Frozen logical-main registers define the header,
so anchor injections cannot change terminal-main identity. Flat15/page16 adds
thread exit codes; kernel model v4 includes logical syscall-visible IDs.

The bounded profile requires explicit raw sharing flags, no inherited clear-tid or
rseq for birth, and no robust ownership at exit. It does not claim pthread support.
All-node ownership/stop preflight precedes restore; physical dirty flags force a
full rollback after failed guest execution or partial restore. Entry cancellation
skips a pending syscall before restore injection; capture refuses such a phase.
C02 remains partial for full Linux lifecycle; C03/C25 actor and instruction-level
coverage remain open despite the explicit transition actor API.
