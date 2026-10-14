# Correctness backlog

These are source-supported findings, not all reproduced end to end. Component/build
checks do not close these items. Priority reflects risk to exploration soundness.
Use one item per focused change, with a failing regression before the fix.

| ID | Priority | Evidence / problem | Evidence required to close |
| --- | --- | --- | --- |
| C01 | P0 | `tracee_state(int, info)` initializes `futex_state` to null without capturing the live wait queues; default copy shares the raw pointer. `thread_blocked` is separate runtime state and is not reconstructed by snapshot restore. | Save a waiting thread, wake it in branch A, restore and execute branch B; queues and runnability must match the saved state and copies must not alias. Include disk round trip. |
| C02 | P0 | `save_full_state_to_state_store` clears/re-enumerates threads, while `remove_thread` promises stable slots. Physical TIDs are serialized into hashed state. | Create A/B, exit A, create C, repeatedly restore; virtual identity remains stable, kernel TIDs are remapped, equivalent logical states have the same identity. |
| C03 | P0 | New BFS sets a selected thread before `state_transition`; recovery and `execute_step` synchronization can overwrite that selection. Two exploration implementations remain compiled; CLI uses the old one. | A two-thread finite program exposes both successor transitions from one snapshot. Trace all callers, compare BFS/DFS results, then consolidate to one path. |
| C04 | P0 | `sys_state` combines child hashes by XOR, without including status/error bound in that combination. Storage and visited-state logic trust this key. | Test ordered node states, equal child hashes, differing terminal status and error budgets; document any intended symmetry reduction and enforce semantic equality before merging. |
| C05 | Fixed (scoped) | Removed filename heuristics; corrected libelf string-table index in Go-section detection. | Native `test_probe` starts at the same boundary as `probe`; ordinary/Go-section ELF fixtures and two existing gc Go binaries classified correctly. gccgo and actual Go runtime execution remain unverified. |
| C06 | P1 | `state_to_be_discarded` reads Raft symbols and cuts paths in the generic scheduler; Raft metadata/parser also lives in core. | Generic workload search independent of symbol names; opt-in Raft checks through the existing plugin boundary, with the same intended counterexamples. |
| C07 | P1 | `read_mem` substitutes live tracee memory when saved data is missing/invalid. | Unavailable/corrupt snapshot produces an explicit failure, never a plausible value from another state. |
| C08 | P1 / partial | Complete RegionInfo initialization fixes demonstrated same-edge hash drift; removed local madvise of remote addresses. General store/canonicalization equivalence remains unproven. | NP=1 counter values and successor hashes survive three restore/replay cycles. Still need cache eviction/disk reload, short I/O, mapping changes, multiple nodes and a simple-path oracle. |
| C09 | Fixed (scoped) | Replaced process-group termination with owned-child kill/reap; signal handler avoids store shutdown locks. | NP=1 scenarios verify unrelated sibling survives both normal exit and SIGTERM, and SIGTERM reaps tracees. Fork descendants, multithread races and initialization failures need additional coverage. |
| C10 | Fixed | Migrated removed libdwarf calls to supported APIs; validated with installed libdwarf 0.11.1 and GCC 14.2. | Real production parser tests pass DWARF 4/5, two CUs, sizes/member offsets, symbols, arrays and cleanup/reinit; tracer compiles and links. Split DWARF and PIE relocation remain outside coverage. |
| C11 | Fixed (distinct addresses) | Direct `futex_requeue` now updates `waiter_map_` when moving a waiter. | Both entry points agree on queue/address; archive round trip followed by `wake_thread` succeeds. Same-address requeue and full syscall ABI still need work (C16). |

| C12 | Fixed | `handle_exit_group` wrote a raw wait status without the DetSim exited flag; successful exit became “running” and the next step crashed. | Finite program reaches counter 1/2/3, exits virtually, and further stepping reports exit status 0. |
| C13 | Fixed | BFS reset an optional on violation, continued executing and printed a false completion message. | Reachable `counter < 3` violation interrupts BFS without printing completion; ordinary finite BFS explores the expected five states. |
| C14 | Fixed | Batch command parser pointed at the NUL inserted by strtok and discarded all arguments. | Real batch handles whitespace, `sw 0`, `p counter` and `q`. |
| C15 | P1 | Standard streams are absent from VFS open_files; intercepted `write(1, ...)` returns `-EBADF`. The counter fixture ignores that return value. | Define deterministic stdin/stdout/stderr semantics and test writes, errors, close and restore; do not silently pass through external I/O. |
| C16 | P0 | Futex syscall handler clears blocked flags for remaining waiters rather than the woken ones. Requeue decoding/comparison semantics are incomplete; direct same-address requeue can invalidate vector references. | Wait/wake and requeue tests at the syscall boundary, including blocked-state updates, actual argument layout, same-address cases and snapshot branches. |
| C17 | P1 | CPU-feature patch logs “Symbol not found” on ordinary dynamic workloads because the requested loader symbol is not resolved. | Define and test which nondeterministic CPU/runtime state must be normalized; avoid assuming this patch worked from a successful scenario. |

## Next development slice

Redis smoke evidence (2026-10-14): [redis-smoke.md](redis-smoke.md).
Redis findings and resolution:

- **C18 / fixed:** Indexed type definitions once, removed nested CU scans, and
  guarded recursive aliases. Forward/opaque same-name typedef and C++ class4/5
  regressions pass; original Redis debug initialization takes about 0.34 seconds
  and 64 MiB RSS under a 1-GiB cap (previously >45 seconds / 6.2 GiB sampled RSS).
- **C19 / fixed for the supported path:** Checked guest sockaddr/length copies,
  length writeback, truncation, fd/peer matching and stale queue handling pass 12
  cross-process cases; original Redis SET/GET now completes. Snapshot representation
  is unchanged. Ambiguous multiple same-fd peers explicitly return EOPNOTSUPP.
  EFAULT retains the virtual pending connection; partial copyout is not rolled back.
  These error-path modeling limits and accept4 flag fidelity need further coverage.

Use the now-runnable tracer scenarios to fix C01/C16 with a minimal two-thread
wait/wake workload. Protect virtual thread identity before consolidating scheduler
implementations (C02/C03). Specify the standard-stream model (C15), then extend
state-store verification (C08) to eviction, partial I/O and memory-map changes.

Current scenario evidence is NP=1, native non-PIE, single-thread, with inherited
ASLR unchanged. It verifies repeated restore within one live process, not cross-run
canonical hashes, multi-node symmetry, complete syscall fidelity or general search
soundness. The early single-thread commit (`c21c496`) remains a design reference,
not a proven oracle.
