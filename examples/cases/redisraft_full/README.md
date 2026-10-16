# Actual Redis + RedisRaft module case

This target runs three unmodified, dynamically linked Redis 7.4.2 servers with
RedisLabs RedisRaft module source commit
`ade4aa8e6aa5c3b21678a1998309825f06567d4f`. It is distinct from the standalone
RedisLabs Raft library target. A fourth process is the real C RESP client, so the
model requires an immutable **NP4** tracer.

```sh
python3 examples/cases/redisraft_full_run.py --native-only
```

The runner copies the already available module source and dependencies under its
unique ignored build directory, then uses the installed CMake and system GCC to
build Debug modules. It downloads nothing and changes no vendor files. The
healthy module has no semantic source changes. The mutant changes exactly one
`RAFT_LOGTYPE_NORMAL` switch arm: node ID 3 skips its first non-client-attached
`executeLogEntry`, then follows the existing common applied-index update and
success return. The static once flag is ordinary guest program data. Threads,
fsync and quorum reads retain normal behavior. Log rotation is disabled with
`raft.log-max-file-size=0`; Raft snapshot creation is disabled with
`raft.snapshot-disable=yes`. DetSim machine snapshots remain required.

The C client connects to all servers, initializes node 1, joins IDs 2 and 3,
checks all members are voting, and obtains separate `+OK` acknowledgements for
`SET alpha 17` and `SET beta 29`. Ordinary `GET beta` requests use the module's
quorum-read path (`src/redisraft.c`: `raft_recv_read_request`) to generate real
network progress. There are no sleeps or artificial epoll timeout results.
`INFO raft` establishes per-node term, commit and applied thresholds. Only
`RAFT.DEBUG EXEC GET` counts as a node-local read: a redirected quorum GET does
not establish local application. The client emits bounded version-1 JSON lines;
the same client exits 0 on checked success or 42 on an observed missing value.
Native mutant acceptance requires precisely node 3 missing alpha, node 3 having
beta, both acknowledged values, and all three applied/commit thresholds reached.
Timeouts, crashes, missing stages or failed process cleanup remain non-green.

The checker includes only the public plugin ABI. It reads captured client stdout
through `read_buffer(FILE)` and discovers each configured log descriptor through
`inspect_json.open_files`; it never reads module globals, private runtime heaps,
live memory or host paths. Every log RESP field, including integers, is bulk.
The header and each complete record have a checked CRC32C chain local to that
file. Headers differ by node ID, so CRCs are never compared between nodes.
Committed entries are compared by index, term, ID, session, type and every binary
command byte. Missing records at an observed committed/acknowledged threshold
are unavailable, never passing; an incomplete uncommitted tail remains pending.
Compacted prefixes are unavailable in this deliberately uncompacted profile.
The original initializer legitimately uses header prev-term 1 with prev-index 0
on the first node, while joiners start with 0/0.

Canonical bounded monitor bytes own the consumed output prefix, ACK indices,
term/commit/applied observations, local observations and exact committed entries.
They enforce branch isolation and rollback checks. The standalone checker test
covers per-node CRC chains, binary command conflicts, saved-data failures,
partial tails, malformed monitors, ACK/commit rollback and repeated monitor
roundtrips. It also verifies the actual native output and persisted log files.

Model mode first controls real startup and membership requests up to the saved
`before_set(alpha)` observation, before the first SET is sent. It then starts a
bounded, case-side beam-1 search from that prefix. It never controls the skipped
apply manually and searches only its later observation.

Each expanded source is restored before every candidate. Fresh public CLI thread
listings enumerate every runnable `(process, logical slot, normal preset 0 or 1)`
and every finite waiting `(process, logical slot, timeout choice -2)`. The explicit
TCP profile configures gettimeofday syscall 96 with two public ChoosePoint
outcomes. `time_choice.cpp` supplies hold0 and +10000 microseconds1 through the
public `choose_time` ABI, with normalized source/carry/overflow checks, cost0 and
no persistent mutable state or private workload inspection. Other syscalls can
ignore these presets and reach the same hash; `duplicate_successor_edges` records
aliases, which never count as distinct successors. Dead and untimed waiting actors
have no transition candidate. Each candidate gets a private copy of the source's
Python observations; the restored product monitor remains the property oracle.
All candidates are checked, including siblings that will be pruned. Exactly one
continuation prioritizes normal runnable actors, rotates a deterministic cursor
over `(process, logical slot)` and uses positive preset1 when configured. Only
when no normal candidate exists does it retain a timeout in actor round-robin
order. Every legal hold and timeout sibling is still executed and checked; their
unretained frontiers are pruned. The guide is independent of variant, fault or
property, and its exact policy string is validated by matrix acceptance. The selected path
and every candidate's actual source/action/target are recorded independently of
TSS history, which retains only the first incoming edge for a target.

This is a bounded search with discarded frontiers, not exhaustive DFS or a
fairness/liveness proof. `search.json` records depth/wall bounds, expanded layers,
attempted edges, distinct successors, pruned successors, partial-layer status,
selection cursor, chosen path and termination reason. The search wall budget
constrains CLI waits as well as action/replay work: every command is checked before
and after return, and the original session wait budget is restored afterward.
`elapsed_seconds`, the effective remaining wall allowance and `wall_cutoff` are
recorded; even a third replay returning after the deadline cannot pass.
A successful row requires
at least two distinct successors from one expanded source. Depth/wall cutoffs,
cycles, no legal actions, unknown outcomes and unavailable inspection stay
non-green. No manual-fault fallback exists.

Healthy completion requires both exact SET ACKs, all three commit/applied
checkpoints, six unique correct node-local values, `done` and virtual client exit
0. A mutant witness requires the exact product diagnostic for node 3 missing
alpha, both ACKs, all three checkpoints, the four correct node 1/2 cells and the
unique missing node-3-alpha cell. The product monitor stops on that first bad
observation, so the model witness has not read node 3 beta. The native prerequisite
independently checks all six cells, including correct node 3 beta. Any other
violation is a failure. The actual terminal or completion edge is replayed three
times from its recorded source, requiring equal target hash, verdict, diagnostic,
observations and virtual client exit.

To run the requested pages subset with a matching immutable NP4 tracer:

```sh
python3 examples/cases/redisraft_full_run.py \
  --variant healthy --backend pages --tracer build/case-tools/tracer-iteration14b-np4 \
  --reuse-modules build/redisraft-full-cases/run-o6dwejfc \
  --timeout 1800 --max-control-steps 6000 \
  --search-depth 2000 --search-seconds 1200
```

`--reuse-modules` verifies module hashes against their original manifests and
avoids rebuilding the external module. The runner separately builds/tests the
small time-policy plugin. Model and native report contexts record its binary and
source hashes plus the declared clock input profile; matrix acceptance requires
those inputs to match. The native run still uses the host clock: recording this
context does not apply the model clock schedule to Redis outside DetSim.
`--native-only` runs only the native
prerequisites. `passed` and `requested_matrix_complete` refer to the exact unique
requested rows; requesting one backend or variant cannot set
`model_matrix_complete`. That flag requires healthy and mutant in full and pages,
plus both native prerequisites, all with validated business/replay/cleanup
evidence. Acceptance binds `search.root` to the row's canonical 16-hex pre-SET
prefix with no ACK yet. A nonempty continuous path must reference actual selected
candidate edges by index, source/action/target and layer, and terminate at the
witness. Accumulated path observations and virtual exit must equal its business
evidence. Detached witnesses, broken paths and missing/late wall evidence fail.
Missing, duplicate, unexpected or incomplete rows fail acceptance.
The full RedisRaft model matrix remains unverified until those actual runs pass.

The prefix pump reads each process's public CLI `thread` listing afresh every
round. Server startup through readiness uses explicit `si 0`; membership/business
normal steps then use `thread N; si 1`, so real gettimeofday opportunities advance
saved time even while GET/INFO traffic remains runnable. Every normal action
specifies a preset again because a non-time syscall may ignore it. RedisRaft
fsync workers therefore receive explicit steps. The CLI derives
`state=runnable|waiting|exited` from saved alive/pending state: Waiting is waiting;
Retry, Ready and no pending call are runnable; dead actors are exited. Waiting
with a finite saved absolute deadline additionally shows
`deadline=SECONDS.NNNNNNNNN`. Missing/unknown state or malformed/noncanonical
fields fail parsing rather than silently dropping actors.

When a prefix round completes no action, the controller refreshes all lists. If
there is no runnable actor, it chooses one finite wait by `(saved deadline,
process index, logical slot)` and issues `si timeout`. Deadlines belong to
per-process model clocks; numerical ordering is a fixed control policy, not a
claim of shared global time. Untimed waits are never assigned a deadline. No
runnable actor and no finite wait fails explicitly. The search instead enumerates
all admitted normal presets and timeout candidates at every expanded source.
These per-process clock inputs are a disclosed environment policy, not a global
clock claim or an artificial epoll success.

`control-events.json` and search edges distinguish attempts from completed actions.
Completed actions have `executed=true`, `status=completed` and predecessor/
successor hashes. A blocked CLI reply records `executed=false`, `status=blocked`,
no successor and an `observed_hash` that must equal the source. Errors retain the
attempted actor, reply and `status=error`. A timeout requires the current reply
`Timeout completed.` and a changed hash; history's `(timeout)` marker never proves
which action this attempt completed. Client assertion diagnostics fail immediately.
All model runs retain config/source/binary hashes and process cleanup inventory.

Local verification commands:

```sh
python3 examples/cases/redisraft_full/control_test.py
python3 examples/cases/redisraft_full/native_result_test.py
python3 examples/cases/redisraft_full/search_test.py
python3 tests/guided_search_integration.py --tracer build/case-tools/tracer-iteration14b-np1
```

The time-policy callback also has a standalone ABI test, `time_choice_test.cpp`,
compiled and run by the runner before native/model cases. The finite integration
fixture has no time plugin/ChoosePoint profile, keeps only normal preset0, and
compares two real runnable logical actors against
manual CLI successor sets in full and pages, repeating after alternate-branch
restores. Its builtin DFS observation is reported separately; C03 actor coverage
is not closed by the case enumerator.
