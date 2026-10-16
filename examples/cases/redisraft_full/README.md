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

Model completion is a separate stage. It must first reach a recorded business
prefix by controlled execution, then run a genuinely nonempty bounded search and
replay the saved violation edge three times. A search from the final local-read
observation predecessor only reproduces observation of an earlier fault; it must
not be described as discovering the earlier skipped apply itself. Initializing
servers or passing the native/checker tests does not complete the NP4 full/pages
matrix. The completed native matrix requires explicit `--native-only`. To reproduce the
first controlled model blocker with the currently supplied NP4 tracer:

```sh
python3 examples/cases/redisraft_full_run.py \
  --variant healthy --backend pages \
  --tracer build/case-tools/tracer-iteration11-np4 \
  --reuse-modules build/redisraft-full-cases/run-o6dwejfc \
  --timeout 180 --max-control-steps 1500
```

`--reuse-modules` is optional and checks immutable module hashes against their
original build manifests. Without it, the runner builds new source copies. The
controller steps each real server main actor to its ready message. Each membership
pump round reads each process's public CLI `thread` listing and explicitly selects
every logical thread slot with `thread N` before `si 0`. The `0` is a syscall
choice, not a thread selector; RedisRaft background fsync workers therefore receive
explicit steps. Newly listed slots participate on the next round. Actor identity
is process index plus logical slot, never host TID. This fixed fair pump does not
search the startup sequence, exhaust schedules or synthesize timer expiry. Client
assertion diagnostics immediately fail the case before following the abort path.
The original iteration-11 run rejected `unlink` (syscall 87) during server 1
initialization; current run artifacts determine the current blocker.
`control-events.json` records node, logical thread slot and syscall choice for each
attempt. Completed syscall steps have `executed=true`, `status=completed` and
predecessor/successor hashes. A CLI "Thread N is blocked (pending syscall). Cannot
step." reply instead records `executed=false`, `status=blocked` and an
`observed_hash` that must equal the predecessor; it has no successor edge. Errors
retain the attempted actor with `executed=false`, `status=error` and the reply; results retain the phase, exact predecessor hash, last replies,
configuration/hash and the inventory of owned processes after cleanup. A reached
pre-SET prefix also remains explicitly incomplete until bounded fault-event
search and replay are implemented and pass. Blocked states, timer deadlines and
unsupported operations remain failures for the generic controller to address.
