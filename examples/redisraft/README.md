# RedisLabs Raft library

`raft/` is the pinned Raft C library, not the complete RedisRaft Redis module.
`redisraft_fixture.c` drives three nodes; `redisraft_repair_fixture.c` adds the
log-repair workload. Existing check/clock plugins and JSON configurations remain
beside their library. `run.py` builds owned healthy/mutant copies and leaves the
submodule untouched.

```sh
python3 examples/redisraft/run.py --tracer ./tracer --backend all
python3 examples/redisraft/quorum.py --tracer ./tracer --backend all
python3 examples/redisraft/repair.py --tracer ./tracer --backend all
```

Each controlled entrypoint requires NP=3 and also supports `--native-only`.
`run.py`, `quorum.py` and `repair.py` own their election, replication-quorum and
log-repair configuration/acceptance policy. `search.py` holds this case's fixed
three-node sibling search, Raft log observation, ledger and replay validation;
its bounded beam policy is not native exhaustive BFS/DFS.

Each session writes its exact `<session>/config.json`; source and tool hashes,
native oracles, observations, transcripts and three-replay evidence go beneath
the selected `--output` directory. The actual Redis module/client workload lives
separately in `../redisraft_module/`.

## Profiling and snapshot comparisons

`benchmark.py` controls the existing three-node library driver and measures its
manual prefix followed by native DFS. `seed.c` supplies the optional benchmark
seed interposer; it is not part of ordinary case semantics. The
`benchmark-redisraft` make target builds case plugins under
`build/<mode>/np3/examples/redisraft/` and runs this benchmark.

```sh
make benchmark-redisraft CC=/usr/bin/gcc CXX=/usr/bin/g++
python3 examples/redisraft/page_snapshot_benchmark.py --tracer ./tracer --repeats 3
python3 examples/redisraft/profile_summary.py PATH_TO_RESULT_JSON PATH_TO_PERF_STACKS
```

The default comparison plugin directory is
`build/release/np3/examples/redisraft`; pass `--plugin-dir` for another build mode.
Its files are `choose.so`, `check.so` and `seed.so`. Comparison runners invoke
this directory's `benchmark.py` and leave exact per-run configuration, outputs
and measurements under the selected `--output`. `profile_summary.py` attributes
recorded DFS-window CPU samples, not wall-clock shares.

`soft_dirty_benchmark.py` and the page comparison's optional `--soft-dirty` flag
retain the historical explicit unsafe delta experiment. They do not establish
sound incremental capture and are not enabled by the normal case runner.
