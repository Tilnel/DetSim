# Correctness backlog

These are source-supported findings, not all reproduced end to end. Component/build
checks do not close these items. Priority reflects risk to exploration soundness.
Use one item per focused change, with a failing regression before the fix.
The [state/atom contract assessment](model-checking-contract.md) explains their
relationship to the native-binary model-checking goal.

| ID | Priority | Evidence / problem | Evidence required to close |
| --- | --- | --- | --- |
| C01 | Fixed (unchanged live thread set) | Futex queues are branch-owned, captured and archived; restore reconstructs blocked state, including same-hash restores. | `check-futex-snapshot` exercises capture/copy/assignment/archive, persisted reload and repeated ptrace restore/wake. Raw lifecycle/carrier restore has separate C02 tests; combined lifecycle and futex waiting still needs wider coverage. |
| C02 | P0 / partial | Capture preserves virtual slots and creation records; raw clone/exit now restore logical contexts over retained carriers; unexpected physical death or untracked creation rejects. Physical TIDs now stay in transient bindings; explicit liveness and logical parent IDs enter archives. Guest-visible physical IDs in register/memory contents are not normalized. | Create A/B, exit A, create C, repeatedly restore with correct logical identity and TLS/stop phases. `check-thread-lifecycle` now covers that sequence, TLS, persisted replay, main-first exit and actual carrier reuse. TID writeback/clear-tid now have join and replay coverage; ordinary pthread TLS/join/condition and non-PI robust owner-death have scoped coverage; full pthread, PI and rseq critical sections remain unsupported. |
| C03 | P0 | New BFS sets a selected thread before `state_transition`; recovery and `execute_step` synchronization can overwrite that selection. Two exploration implementations remain compiled; CLI uses the old one. | A two-thread finite program exposes both successor transitions from one snapshot. Trace all callers, compare BFS/DFS results, then consolidate to one path. |
| C04 | Partial | Ordered system metadata identity includes status/budget; storage now compares duplicate bytes in cache, packed records and metadata instead of trusting hashes alone. | Identity and all storage gates cover ordering and duplicate conflicts. Logical thread identity and equivalence of differently encoded semantic states remain open. |
| C05 | Fixed (scoped) | Removed filename heuristics; corrected libelf string-table index in Go-section detection. | Native `test_probe` starts at the same boundary as `probe`; ordinary/Go-section ELF fixtures and two existing gc Go binaries classified correctly. gccgo and actual Go runtime execution remain unverified. |
| C06 | Fixed (framework boundary) | Removed Raft fields, implicit role pruning, message inference and example parser from the production link. Versioned plugins inspect explicit snapshots; branch-owned opaque monitors enter product identity before successor publication. | `check-state-view`, `check-plugins`, `check-identity` and native counter-plugin restore/violation scenarios cover readonly inspection, ABI failures, history isolation, terminal witnesses and decoder noninterference. Raft examples have separate projection/history contracts; this does not certify every Raft property. |
| C07 | Fixed | Snapshot reads now require saved bytes and reject missing/corrupt data; no live-memory substitution. | `check-state-io` covers missing/invalid archives and reads spanning saved regions. |
| C08 | Partial | Native same-edge replay preserves hash; storage tests now cover immutable reader lifetime, cache pressure/zero budgets, short I/O, prefetch and fresh-process reload. | `check-storage`, `check-state-io`, `check-capture` full-byte oracle, mapping and native replay gates pass. Full exploration equivalence across cache policies and general multi-node semantic canonicalization remain unproven. Virtual-exit/fatal-stop branch closure now has real ptrace regressions: persisted terminated images load and restore along with live peers; recapture preserves identity. Physical process death still rejects; lifecycle reconstruction remains C02. |
| C09 | Fixed (scoped) | Replaced process-group termination with owned-child kill/reap; signal handler avoids store shutdown locks. | NP=1 scenarios verify unrelated sibling survival and full/pages pthread normal/SIGTERM cleanup with all owned TIDs gone. Dedicated host groups let cleanup reap parked workers before leader. Unsupported prelude group changes reject. Fork descendants and broader initialization races need additional coverage. |
| C10 | Fixed | Migrated removed libdwarf calls to supported APIs; validated with installed libdwarf 0.11.1 and GCC 14.2. | Real production parser tests pass DWARF 4/5, two CUs, sizes/member offsets, symbols, arrays and cleanup/reinit; tracer compiles and links. Split DWARF and PIE relocation remain outside coverage. |
| C11 | Fixed (distinct addresses) | Direct `futex_requeue` now updates `waiter_map_` when moving a waiter. | Both entry points agree on queue/address; archive round trip followed by `wake_thread` succeeds. Same-address and syscall requeue ABI now have C16 regressions. |
| C12 | Fixed | `handle_exit_group` wrote a raw wait status without the DetSim exited flag; successful exit became “running” and the next step crashed. | Finite program reaches counter 1/2/3, exits virtually, and further stepping reports exit status 0. |
| C13 | Fixed (BFS/DFS/RAND) | BFS formerly continued after violation; DFS ignored its stop code and RAND only broke an inner loop. All now stop the entire search, retaining a failed product-state witness. | Native scenarios verify reachable assertion violations stop each mode without a false completion; counter-plugin replay preserves terminal identity. Ordinary finite BFS still explores the expected five states. |
| C14 | Fixed | Batch command parser pointed at the NUL inserted by strtok and discarded all arguments. | Real batch handles whitespace, `sw 0`, `p counter` and `q`. |
| C15 | Fixed (captured streams) | STDIO descriptors share the virtual FD registry; stdin is deterministic EOF and stdout/stderr content is archived VFS state. | `check-io-boundary` covers access modes, writes, close, archive and branch isolation; terminal input/output and interactive stdin remain unsupported. |
| C16 | Fixed (scoped) | WAKE unblocks removed waiters, WAIT saves an unfinished successor; wake grants a separately published return; requeue uses arg4 count/arg6 comparison, returns total affected and handles same address safely. | `check-futex` covers real ptrace WAIT/WAKE/bitset/requeue/errors; timed wait deadlines and explicit expiry are separately tested; PI operations explicitly fail. Exhaustive wake-order outcomes remain outside coverage. |
| C17 | P1 | The glibc-private CPU-feature patch has been removed; general nondeterministic CPU instructions/runtime inputs still need a supported execution contract. | Define and test which nondeterministic CPU/runtime state must be normalized; avoid assuming this patch worked from a successful scenario. |
| C20 | Fixed (host user XSTATE) | Capture/archive/restore NT_X86_XSTATE for each thread; snapshot version 5 rejects old formats. | `check-xstate` covers XMM/AVX/x87/MXCSR; native SIMD scenario repeatedly restores observed=0 and replays observed=11. Cross-host layouts and supervisor state are outside coverage. |
| C21 | Fixed (scoped) | Save private readable mappings and unnamed anonymous guards including hidden bytes; restore protections/backing and canonicalize equivalent VMA partitions. Reject shared and other unreadable maps and missing file/named backing. | `check-mappings` covers RO contents, permissions, split/merge, anonymous recreation, explicit unsupported cases and wide map fields. Native replay preserves successor identity. |
| C22 | Fixed (snapshot I/O) | Exact read/write counts and archive bounds required; failures propagate, without zero-fill or live fallback. | `check-state-io` fault-injects short/failed process_vm I/O and corrupt/missing/old archives. Failed restore is fatal; no successor may execute afterward. |
| C23 | P0 / partial | TCP source ports now derive from saved socket state, independent of search RNG; clocks/getrandom now use saved virtual state; per-task masks now persist; custom signal delivery and asynchronous kernel inputs remain unsupported. | `check-port` covers replay, RNG independence, source addresses, uniqueness and exhaustion. Closed syscall policy and process-model regressions cover rejection, deterministic replay and archives; this does not close general asynchronous inputs. |
| C24 | Fixed | Populate current syscall number/arguments before the entry handler. | `check-exec` exercises real ptrace FUTEX_WAKE suppression using current arguments. |
| C25 | P0 / partial | Auto execution returns after each syscall boundary or explicit pending completion, including CKPT_NO. Runnable threads/outcomes are still not exhaustively enumerated; pure user loops and blocking kernel calls are not bounded. | `check-exec` verifies empty receive, sleep and getpid loops return control. Futex waiting successors are now preserved; enabled actor enumeration and shared-memory atom boundaries remain open. |
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

The original terminal-image slice retained descriptors. Normal last-thread/group
exit now closes supported modeled resources; fatal-crash teardown and TCP reset
cases remain outside that later extension. The
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

## Cross-runtime follow-up

The [Linux task contract audit](development.md#runtime-independent-task-contract-audit-2026-10-15)
separates kernel ABI coverage from user-level scheduler coverage. Next bounded
slice: clone/clone3 TID copyout + modeled clear-tid/join, with initial-thread
registration handled from exec rather than the prelude's114514 return shortcut.
Guard mappings, per-task masks/altstack, timed waits, robust and signal delivery
remain generic capabilities. The rseq whole-region masking/live-byte preservation
shortcut remains unverified semantic debt, including on the single-thread path;
raw clone currently rejects rseq rather than certifying that shortcut.

## TID/clear-tid slice completed

clone3 and clone now share TID birth/clear semantics, with explicit pending child
write state and initial-thread identity handled from syscall entry. The prelude
114514 return and uncontrolled thread creation path are removed. Shared/private
futex waiters are distinct and persist across wake/requeue/restore. Native-oracle
and full/pages replay tests pass; format17/18 and kernel modelv5 reject prior
execution contexts. Parent-first syscall atoms and fixed group cleanup order
remain scoped. The next observed blocker for unmodified pthread is rt_sigprocmask;
guard mappings, rseq policy, robust ownership and full signal delivery remain open.

## Per-task mask slice completed

Logical task masks now have capture/copy/archive/identity/restore coverage,
including initial inheritance, clone from a non-anchor task, carrier reuse and
same-hash restore. `rt_sigprocmask` uses native argument/error ordering, including
committing a new mask before an oldset copyout fault. Flat19/page20 and kernel
model v6 record this boundary. The eight signal-mask backend/cache/pending
variants and eight clone/clone3 TID variants pass. Actual pending bits (thread
and shared) reject before restore; no pending queue is silently discarded.
General signal delivery, altstack, timed/temporary-mask waits, cancellation and
external asynchronous input remain open. This is not complete pthread/Go support.

## Anonymous guard follow-up

C21 now includes private unnamed anonymous PROT_NONE capture and restore. Six
full/pages/verify-pages x normal/tiny-cache regressions cover hidden-byte identity,
persisted reload, mapping recreation, permission-only restore and exact-read
failure without root publication. Guest copies retain native permission checks.
Read errors are never replaced with zeros or live memory. Dispatcher guard
mprotect preflights backing before kernel effects; unsupported file/shared maps
remain rejected. Snapshot layout remains19/20; kernel model isv7.

The pthread smoke now stops at active rseq registration during clone. That existing
whole-region rseq masking/live-byte preservation shortcut still needs a real
per-task kernel ABI contract; guard support does not resolve it. Large guard
reservations currently require full hidden-byte reads and full restore writes.

## rseq unavailable policy (2026-10-16)

C02/C08 no longer rely on whole-region rseq masking or live-byte preservation.
The execution contract exposes rseq as unavailable (ENOSYS), consistently from
exec prelude through runtime. Kernel model v8 records that policy; full/page
layouts remain19/20. Ordinary unregistered memory is preserved byte-for-byte.

RED: a real registration previously allowed capture and snapshot publication.
GREEN: main/worker/spare registrations reject capture, same/different-state
restore and direct carrier/low-level restore APIs without state mutation. Four
full/pages/cache lifecycle variants verify these boundaries and exact archived
rseq-shaped memory. Startup and syscall-policy tests cover valid/invalid/unregister
arguments returning ENOSYS without memory writes or host registration.

This closes the old masking/preservation shortcut, not full rseq semantics.
CPU/node/mm-cid, restartable critical-section abort and signal/preemption outcomes
remain unimplemented. The actual pthread probe's next blocker is set_robust_list;
robust-list ownership and owner-death need a separate generic Linux ABI slice.

## Robust lifecycle, discard and cleanup follow-up (2026-10-16)

C02 now includes logical robust-list registration and ordinary non-PI owner-death;
physical registrations are null. Native oracles and four backend/cache variants
cover pending, broken/cyclic/aliased links, private/shared wake separation, group
exit and subsequent PI rejection without mutation, empty-only bootstrap adoption,
carrier reuse and persisted replay. Single-field archive identity is checked.

C21 adds unnamed private anonymous MADV_DONTNEED. A reproduced incorrect admission
of VFS file mappings is fixed by archived canonical provenance intervals, trimmed
on successful mmap/munmap/brk changes. Native dispatcher and complete capture/
restore tests preserve rejection after branch reload. Mutable-file coherence,
file-backed discard and other advice remain unsupported.

C09 adds the reproduced ptraced-worker shutdown deadlock fix. Dedicated owned host
groups are created under a SIGTERM startup barrier, and cleanup reaps all members
without group-wide signals or waiting on unrelated children. Startup cannot change
PGID/session or fork children. Fifteen native scenarios include a real pthread
TLS/join/robust-mutex/condition program under both snapshot backends, with normal
and SIGTERM cleanup proving all retained TIDs disappear.

Flat21/page22 and kernel modelv9 encode the new semantic fields. These results do
not close C03/C25: current scheduling still does not enumerate every enabled task,
memory-access boundary, wake outcome or weak-memory behavior.

## Generic pending syscall slice (2026-10-16)

C01/C16/C25 now separate syscall admission, blocking, readiness and completion.
Waiting/Retry/Ready plus original scalar arguments and copied normalized deadline
belong to each logical thread. Physically skipped syscall-exit is a carrier stop,
not a guest return; no user instruction runs on the completion edge. Futex queues
and blocked caches are validated against this state, not independent truth.

Pipe/socket reads, pipe capacity waits, recvfrom, accept and epoll can suspend.
Readiness only permits retry; output faults and competing consumption do not lose
payload. EOF after supported normal peer exit is an explicit completion. Finite
futex/epoll/sleep deadlines can be expired by the control API after modeled time
is due, without automatic time advancement or scheduling policy.

ABI2 event phases distinguish unfinished calls, actual returns and nonreturning
exit/crash. CLI and StateView expose pending rather than a fake success. The old
per-futex timeout boolean and ad-hoc blocked CLI rollback are removed.
Flat23/page24 and kernelv10 encode this contract.

Regressions cover real stopped carriers, no-instruction completion, Waiting/Ready
and Retry persistence, two read attempts competing for one payload, EFAULT after
buffer protection changes, explicit deadline expiry, last-thread/group EOF, and
full/pages normal/tiny cache equivalence. A native phase-checking plugin verifies
pthread, pipe and fatal-stop event observation. Driver-created competing reads
are not claimed as exhaustive real thread scheduling.

Still open: automatic deadline/actor/outcome enumeration; pending descriptor
close requires pinned open-file identities (currently explicitly rejected);
partial-progress blocking writes; poll/select/vector I/O; transport backpressure;
TCP reset and fatal-crash resource teardown; independent realtime/monotonic axes;
signals/cancellation and shared-memory instruction boundaries. None is silently
covered by the successful controlled executions above.

## Case-driven admission follow-up (2026-10-16)

The case corpus now tests healthy/seeded-bug C and C++ input protocols plus a
closed-world pthread lost notification. Search-found witnesses are restored and
replayed three times with identical local state identity, in full and pages.
This establishes those concrete counterexamples, not C03/C25 exhaustive coverage.

Runtime probes exposed and drove fixes for argv storage (argc5 null terminator
out of bounds; argc9 rejected), ordinary anonymous zero-length mmap probes, and
poll(timeout0). Native regressions preserve file-backed zero-length EBADF before
EINVAL, explicitly reject huge-page setup, and verify poll invalid/negative FDs,
pipe HUP/ERR, epoll POLLRDNORM and cross-page revents EFAULT without a partial
scalar store. Kernel modelv11 records the expanded contract;
snapshot byte layouts remain23/24.

The six installed toolchains all pass native probe execution and two modeled
state transitions with same-process restore/replay. Only C/C++ currently complete
the checked runtime probe and search; Rust std stops at sigaltstack, gccgo at
sched_getaffinity, and CPython/OpenJDK at readlink. These are explicit next nails,
not supported-language claims. Standard gc Go remains separate from gccgo.

Known search-reporting caveat remains: depth-limited DFS prints 'Complete explore
all' and visited state is shared across CLI searches. The new runner uses fresh
tracers per search and labels bounded results conservatively. Future core search
work should fix reporting/coverage directly rather than treating this runner
boundary as closure of C03/C25.

### Runtime queries: captured paths and CPU affinity

The next case-driven slice adds readlink/readlinkat over captured VFS symlink
bytes, including the executable path chain and /proc/self/exe captured during
initialization. No host pathname lookup occurs on a modeled query. Ordinary
symlink traversal remains explicitly unsupported; readlink and nofollow stat
can inspect final links. Missing/non-directory prefixes must be checked before
lexical normalization. The synthetic proc projection does not claim general
procfs support, and observing /proc/self itself as a link remains unsupported.

sched_getaffinity exposes one fixed virtual CPU, validates logical live task
identity and follows the raw x86-64 mask-copy ABI. No mutable affinity state was
added; sched_setaffinity, CPUID/proc/vDSO topology coherence remain outside scope
(C17). Kernel modelv12 distinguishes these semantics; byte layouts stay23/24.
Executable bytes now enter the captured VFS, increasing state-copy/hash work by
binary size; this is a correctness-first tradeoff, not a performance optimization.

Runtime re-audit after these changes: Python now reaches an explicit failed
probe exit (missing captured standard-library files), Java advances to access21,
and gccgo pages reaches sigaltstack131. Full gccgo still fails allocation under
the audit's2GiB address-space limit; a4GiB diagnostic caught a747,734,229-byte
allocation in packed-save decompression. An intermittent pages allocation failure
also occurred before the capacity fix, but was not reproduced in four bounded
GDB runs. None of these results count as runtime admission.

The audit found and fixed compressed-buffer retention: resize(encoded_size)
kept compressBound(raw_size) capacity while warm-cache accounting charged only
encoded_size. A regression now requires compact retained capacity and exact
byte decompression. This does not bound transient raw capture, queued work and
save-side decompression. Executable capture separately regressed the Redis
SET/GET single-run time from7.691s to26.012s; prioritize immutable VFS payloads
without weakening byte equality or branch isolation.

### Shared VFS payload evidence

The executable-import performance regression is addressed by immutable shared
contents and >=4096-byte StateStore references in production metadata. Mutations
have no writable aliases and detach branches; standalone archives remain inline.
Flat25/pages26 distinguish the wire format. Registration memo is generation-bound
and checks store readiness even on hits; first registration still verifies bytes.
No global intern table or host-file fallback is introduced.

check-vfs-content covers sharing, file write/truncate isolation, registered-key
invalidation, restored-buffer detachment, inline archive without store, nested
scope exception recovery, reference identity independent of object sharing,
zero-cache fresh-process/read-only reopen, new store generation, wrong tag/key/
length, first-registration collision and poisoned-store memo rejection. Native
full/pages repeatedly restore an8192-byte file before modification and verify
its original bytes before replaying the mutation. Existing IO/capture/state-view/
plugin/identity/pending-IO/snapshot/storage gates remain green. C08 remains scoped;
these checks do not establish arbitrary exploration equivalence.

Three fixed-schedule Redis SET/GET runs per mode preserve208 server and58 client
steps: full median26.845→7.553s, pages19.506→1.980s. Median stored bytes fall from
2158.6→599.7MiB/full and1577.6→17.3MiB/pages. Full-file COW on first mutation and
raw snapshot transient-memory peaks remain separate limitations.

### Runtime admission iteration: alternate stacks and rootfs

Per-task virtual sigaltstack now archives sp/size/normalized flags, uses active
RSP for ONSTACK, and matches native commit-before-old-copyout/error behavior.
Clone disables child registration; exit clears it; disabled AUTODISARM from the
trusted prelude is imported. Full/pages×normal/tiny replay, carrier reuse, native
partial fault oracles and malformed capture/restore guards pass. Virtual minimum
2048 is explicit kernel profilev13; layout27/28. Physical carriers stay disabled,
inherited enabled stacks and handler delivery remain unsupported.

FSMap now captures actual root file/directory/link type, hidden names and exact
bytes, rejects missing/unreadable/unsupported inputs and invalid ancestor/root
overlays. F_OK access family uses saved paths and rejects credential-dependent
search; R/W/X and generic symlink following remain unsupported. Runtime workloads
provide explicit Python/JDK roots and input manifests, outside generic product.

Rust std probe completes full/pages. gccgo pages now stops at pselect6(270).
Python rootfs exposed a separate DFS ownership defect: ~30 ancestor frames each
retain independently decompressed identical VFS payloads when the files exceed
hot-cache capacity. GDB confirmed30 byte-identical allocations per several large
files and ~2.085GB in use; this is not merely fragmentation. Hash-only ancestor
residency is the next bounded fix. Java stops at ordinary jvm.cfg symlink traversal;
per user instruction, costly JVM-specific work may be deferred explicitly without
blocking other cases, while keeping its status non-green.

### Bounded DFS resident-state fix

The actual CLI iterative DFS now drops an ancestor's resident sys_state before
pushing its successor, retaining hash/actor/choice bookkeeping. It reloads the
same saved identity only when backtracking needs another expansion. No search
choice, visited rule or SIGINT ordering changes. Deserialize failures now retain
the underlying exception rather than hiding allocation failure as generic corruption.

Regression:44 syscall prefix,4x16MiB immutable files exceeding the32MiB hot cache,
late four-way branching and file mutation, unchanged2GiB limit. Old full/pages
large DFS fail while corresponding BFS passes. New full/pages×small/large×BFS/DFS
all eight runs yield81 unique states,80 actual transition events and four final
branches; file-mutation and terminal edges each replay three times with identical
hash. Existing DFS searched82 vs BFS81 reporting discrepancy remains unchanged;
actual event multisets match. Real Python now advances to getdents64(217) in both
backends without the former allocation failure. C03/C25 remain open.

### Timed pselect and captured path traversal

Zero-descriptor pselect6 uses existing Waiting/Ready/deadline state, including
NULL infinite wait and best-effort timeout copyout; fd multiplexing and temporary
sigmasks remain unsupported. Native oracles verify timeout validation priority,
ignored fdset pointers at nfds0 and readonly timeout behavior. Control-page overlap
is rejected at admission and archive validation. No automatic timer expiry added.

Captured symlink traversal now follows at most40 links, checksNAME_MAX, preserves
relative/dotdot/trailing-slash semantics, canonicalizes open descriptions, and
honors nofollow/exclusive creation. Native stat/open/access/readlink matrices plus
archive/host-unlink/branch tests pass. Kernelv14, unchanged layouts27/28.

Rust now has checked endpoint3x replay. gccgo actual history exposes fatal
nanotime returning zero before both threads wait; the model's zero clock origin
needs an explicit generic profile decision.4GiB full run avoids earlierallocation
failure but hits90s bound, not successful admission. Java reaches dlopen(libjvm)
then exits6 with undefined JNI_CreateJavaVM; zero virtual inode metadata is the
next concrete hypothesis. Neither result is counted green.

### Directory streams, file identity and clock origin

getdents64 now enumerates saved names/types with independent serialized ordinal
FD cookies. Native field ordering, untouched padding and completed-record partial
fault behavior are covered, including21/46 writable-byte boundary successes.
Malformed long/NUL namespace names reject before mutation. stat/lstat/fstat and
dirent share nonzero path-derived inodes and reject namespace-wide collisions.
This models path objects while rename/hardlink remain unsupported; it is not
host-hardlink fidelity. Directory insertion may affect ordinal scan position.

A real two-DSO native regression proved zero inode caused the loader to reuse
an unrelated loaded object; current minimal DSOs load independently. Fixture
omits CRT to isolate identity from SIGBUS-backed EOF-tail support, still rejected.
Linux-ignored MAP_DENYWRITE/EXECUTABLE flags have native and model oracles.

Virtual initial_time_us defaults to1s and is explicitly configurable including0;
its value enters model identity. This fixes gccgo's fatal zero nanotime without
automatic time advance or runtime-private patches. Kernelv15, layout27/28.
24 native scenarios,12case rows, getdents/IO/syscall-policy/runtime-profile gates
pass, with independent review and native RED/GREEN evidence.

Python now reaches rename82 because current minimal stat drops imported mtime,
invalidating bytecode caches whose header timestamps actually match host sources.
Preserving semantic mtime is the next direct input-fidelity fix. gccgo pages
completes+3x replay (235visited); full raw run reaches exit0 but original driver
was lost, so full checked-replay admission remains unproven. History rendering
also rereads large snapshots after exploration and in info, a measured next
control-chain cost. Java advances beyond libjvm identity to missing captured
libstdc++ dependency, still non-green rather than JVM admission.


### Captured mtime and observation cost

Captured files/directories/symlinks/executables now retain precise mtime through
copy/archive/identity/restore and stat/fstat, including successful mutation times.
Native utimensat/lstat regression covers1786363619.987654321; archive rejection
covers invalid nanoseconds. Layout29/30 and kernelv16. Independent review found
no state-closure blocker within this scope. Remaining: native write(EFAULT) can
update mtime while the pre-copy model does not (confirmed via ctypes/native
write with pointer1); input capture does not detect concurrent same-size changes.
These are explicit compatibility gaps, not closed by timestamp preservation.

CLI scalar current-edge observation avoids StateStore reads; optional quiet
search suppresses only automatic history and restores command scope on exceptions.
24 actual full/pages BFS/DFS finite/violation/interrupt searches retain transition
multisets and3x replay; rand has component-level scope verification only. Standard
24 native scenarios and12 case rows remain green. Python advances beyond the
mtime-induced rename to FIOCLEX; full gccgo checked admission is running with
explicit4GiB budget. Neither is counted passed until endpoint replay succeeds.

Java's captured ELF dependency closure advances loading to a genuine unsupported
SIGBUS-backed file-mapping case. Supporting invalid-backed pages requires memory
mapping, snapshot and guest-fault semantics, beyond a flag allowlist. Per user
tradeoff Java remains a conditional, explicitly non-green research target; no
claim of JVM admission or private-heap semantic extraction is made.


### Raft workload oracle schema2

The example plugin now records committed term/type/exact command bytes and a
branch-owned term-to-leader history. Identical index/term with different commands,
including embedded NUL bytes, violates the property; sequential different leaders
in one term also violate it. Monitor records are bounded and strictly validated.
Unobserved compacted prefixes remain explicitly unavailable instead of invented.
The schema version participates in the plugin identity. This is example-side
property extraction, not Raft knowledge in generic state storage or scheduling.

Independent review approved the slice; freshly rebuilt `make NP=2
check-raft-plugin-contract CC=/usr/bin/gcc CXX=/usr/bin/g++` passes both actual
entry layouts. Unit and sanitizer evidence is component-level: full/pages actual
workload exploration and replay are separate pending acceptance. Legacy willemt
main has shallow-copied command payload lifetime errors; the owned bounded driver
must retain payload ownership rather than weaken the stronger oracle.


### Generic close-on-exec ioctls and runtime evidence

FIOCLEX/FIONCLEX now reuse saved per-descriptor F_SETFD state, without guest
argument access or host mutation. ioctl commands normalize to unsigned32,
including high-register-bit aliases. Unknown commands still reject. Native RED
then GREEN covers18 oracle cases and108 modeled dispatch calls; copy/archive
isolation and flags remain checked. Independent review approves. Kernelv17,
unchanged layouts29/30. NP2 IO gate and make check pass; production NP1 rebuilt.

Python actual full/pages admission now passes273visited, public output property,
exit0 and terminal predecessor3x replay: build/cases/run-l3jl43u3/results.json.
gccgo full controlled admission passes237visited, terminal3x and cleanup0 with
explicit4096MiB/600s profile (actual entire driver222s):
build/cases/run-yucgcv9q/results.json. This complements earlier pages evidence.

Separately acquired official SHA-verified isolated gc Go1.23.12. Native passes;
munmap startup boundary correctly rejects because it follows thread creation.
An explicit mmap startup boundary admits control/replay, then actual exploration
rejects MADV_NOHUGEPAGE: build/cases/run-wgjcc09h/results.json. This is not gccgo
admission and is not yet green; no GC/preemption flags were disabled.


### Anonymous huge-page performance hint

Admit only MADV_NOHUGEPAGE alongside DONTNEED through existing native execution
and unchanged range/backing restrictions. Physical THP policy is excluded from
this execution profile. No new saved field or snapshot format is introduced;
kernelv18 identifies the profile. Native/kernel errno matrix and high32 advice
aliases pass; wrapping addresses retain explicit model rejection rather than
claiming native EINVAL. Native-file/shared/named/virtual-file/control ranges and
other advice still reject. Full/pages/verify-pages with ordinary/tiny caches
restore both hinted and recreated-unhinted mappings to equivalent bytes,
protections, provenance and successor identity. Independent review approved the
implementation; its requested unhinted-replay test correction is now green.
NP2 syscall-policy/IO/capture and NP1 mapping gates passed. Actual gc Go advances
to getrlimit97; captured resource-limit query closure is the next bounded task.


### willemt paired case acceptance

Owned three-node driver retains log payloads and initializes entry types. Native
healthy/mutant use an exactly-once copied-source vote-response change. Actual
full/pages DFS discovers the intended denied-vote dual leader at term1 and replays
its predecessor edge3x. Healthy all nodes commit two distinct commands, then a
positive-budget bounded suffix searches42states/27unique in each backend; a
post-commit edge restores3x. Evidence: build/raft-cases/run-i1nlq358/result.json.

Independent review caught/rejected the first healthy audit (budget0) and overly
broad mutant diagnostics. Corrected heartbeat opportunities, positive-budget gate,
exact election receipts and9 negative acceptance tests close those runner defects.
The final-state cutoff flag is only an observation of the final state, never proof
that all branches avoided budget cutoffs. This finite case does not close C03/C25
or establish general Raft/snapshot compaction correctness. No vendor tree changed.


### Native runtime concurrency acceptance

Real Rust std thread/mpsc and CPython Condition/list producer-consumer pairs now
exercise normal operation, omitted task and duplicated task. Twelve actual
full/pages rows pass exact protocol/worker/exit checks and final or violation
edge3x replay, without disabling GC or preemption. Rust visits45/41/41 states;
Python410/390/392. Evidence: build/cases/concurrency-0jmhl7i4/report.json (Rust6),
build/cases/concurrency-nuo4il7p/report.json (Condition6). Optional Python Queue
still reports unsupported SIGBUS-backed mappings and remains non-green.

Independent review approved the public-ABI property and native fixtures, then
caught partial-matrix false green after an exception. Exact complete/unique
matrix acceptance and per-combination failure recording now pass7 negative tests.
Existing12actual rows were revalidated without altering evidence. This is bounded
case capability, not full language/runtime or exhaustive interleaving support.
Go gc/gccgo concurrency and OpenJDK conditional target remain separately pending.


### RedisLabs library paired case

The independent RedisLabs Raft-library fixture preserves real pre-vote and entry
reference ownership. Its single copied-source change removes only the actual
vote-grant guard. Native and actual NP3 full/pages pairs pass. Healthy all nodes
commit/apply no-op1 and exact normal commands2/3; bounded suffix70visits/45unique
from positive budget1. Mutants find the intended term1 dual leader on an actual
denied vote and reproduce predecessor→successor3x, with exact diagnostic and
prevote/request-term fields required. Evidence:
build/redisraft-library-cases/run-julvrjq0/result.json. Independent review approves;
four permanent negative acceptance tests cover unrelated failures and invalid
roles/terms/receipts. No vendor changes. Both library pairs are implemented;
the real Redis+RedisRaft module remains a separate unfinished target.

Real module prerequisite progress: acquired official RedisRaft ade4aa8e and
unmodified Redis7.4.2 a0a6f23d into ignored owned build trees. The existing local
Redis is a modified static PIE and crashes natively in module pthread_create;
that is not a DetSim failure. Separately built dynamic Redis plus module passes
native three-node INIT/JOIN/SET and GET via legitimate redirects. Model bootstrap
admits all three but first controlled search explicitly rejects prctl157:
PR_CAPBSET_READ; native tracing also identifies worker PR_SET_NAME. Captured
capability-query and per-thread name semantics are the next generic requirement,
not a workload-specific state-extraction patch.


### Captured resource query state

ProcessKernelState now owns all16 x86-64 resource-limit pairs. Bootstrap imports
inside its preserved scratch transaction and publishes after complete success;
archives validate soft<=hard before committing the entire object. getrlimit and
owned-self prlimit64 share saved-table emulation. Removing native prlimit execution
also removes its live PID rewrite and ambient-query dependency. Native ABI
oracles cover every resource, integer aliases, NULL/error priorities and partial
copy prefixes. Changing only a child's native soft limit after capture leaves
modeled answers unchanged. Copy/archive/identity distinguish single-field changes.

Review caught control-page copyout; both API overlap guards now reject without
modifying page/register/model state, while invalid-resource EINVAL and NULL query
rules remain. Independently approved final source.495 NP2 assertions, full/pages
snapshot/state-I/O gates, identity,24 native scenarios,12 case rows, make check and
production build pass. Layout31/32, kernelv19 reject incompatible roots. Actual gc
Go admission/concurrency now rerun against the new immutable binary. This is
query closure only: no claim of restoring/enforcing physical resource limits,
CPU budgets, complete virtual FD capacities or limit mutations.


### Go admission and goroutine protocol matrices

Standard gc1.23.12 now passes admission in full/pages (201visited each), checked
output/exit0 plus control and terminal3x replay. Explicit mmap startup9 ends the
trusted prelude before thread creation.4GiB profile,900s full admission budget;
actual full driver429s. Evidence: build/cases/run-ixays29s/results.json (pages),
build/cases/run-0xfy5b8i/results.json (full). No GC/preemption flags disabled.

Both gc and gccgo goroutine/channel fixtures now have complete healthy/omit/
duplicate full/pages matrices,12actual rows with native/protocol/exit and3x replay.
gc201/199/199; gccgo full234/236/238 and pages236/236/234. Reports respectively
concurrency-dijy2cw7,concurrency-oschmx18,concurrency-nk79ibko,
concurrency-54sbjxge under build/cases. Independent review approved after closing
compiler identity: explicit -compiler overrides oppositeGOFLAGS;13runner tests,
6opposite-flag native builds and stored-binary compiler/hash audit pass. Evidence:
build/case-jobs/go-compiler-audit-7sy2joyc/report.json. This is language/runtime
protocol capability under current actor policy; Go runtime OS-thread count does
not establish goroutine-to-OS-thread assignment or exhaustive scheduling.


### EOF fault backing: native feasibility evidence only

Independent native oracle tests/file_eof_native_oracle.cpp compares ordinary file
mappings against empty-memfd tail overlays with requested permissions and closed
FDs.912read/write/execute comparisons per run,1824children all reaped, stable FD
count, exact signal/si_code/relative address and normal-page bytes; half-page tail
zero/COW leaves original file unchanged. Native and UBSan runs pass; current
hostmemfd_noexec=0, zero execute skips. Independent review approves evidence.
Clang analysis has no diagnostic; GCC14 fanalyzer fails with a documented apparent
policy_fd false positive despite immediate close, not mislabeled as clean.

This does not close the framework gap. Approved next-slice contract requires
explicit FileEofFault regions, no fabricated zero payload, both readers unavailable
on faults, target-snapshot restore of genuine fault backing, serialized provenance
and fixed-image mutation guards. Python Queue/Java/module claims remain non-green
until actual program execution, property and replay gates pass.


### Captured capability bounds and logical task names

Closed prctl subset: CAPBSET_READ imports native support count+bounding mask once,
with explicit uncaptured0/0 marker; SET_NAME/GET_NAME use canonical per-task16bytes.
Names enter copy/archive/identity/restore, inherit from logical parent before
vector growth, freeze on exit and survive carrier reuse without physical rename.
Native oracles cover option32/caparg64 and SET15/GET16 fault/partial-copy semantics.

Independent review caught two closure bugs before commit: late prelude native
rename was not imported, and first-capture helper could inject before stop barriers.
Final bootstrap now commits its local name only after the entire transaction;
low-level adoption only reads /proc, never resumes a task. New RED/GREEN wrapper
regression verifies first capture with running worker rejects with zero resume
attempts and unchanged registers/control page. The real startup157 scenario checks
both backends through search, controlled GET/output/exit and3x same hashes.

Independent final review approves; NP2process245/policy134/IO244, thread control/
carrier, four lifecycle combinations,25 native scenarios,12case rows, snapshot/
state-I/O, make check and NP1/NP3 builds pass. Layout33/34, kernelv20. This does
not implement arbitrary prctl, capability mutation or dynamic procfs names.

Real module attempt now searches rather than rejecting its capability query:
three actual Redis+module processes, pages DFSdepth500, about5755unique states
at the180s timeout. No business client or property checkpoint yet, cleanup-9;
recorded owner group has no surviving tasks. Evidence:
build/workloads/module-control-1b3dchvf/result.json. This is execution/search
progress only. Next prioritize a controlled startup prefix and real client, not
unbounded startup interleavings; ftruncate is statically identified at log init.
