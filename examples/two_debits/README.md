# Two debits

The C and C++ drivers model two debits from an account with initial balance 1.
`INJECT_BUG` removes the insufficient-funds guard; `input_checker.cpp` checks
nonnegative balance. Sources are built into an owned output directory.

```sh
python3 examples/two_debits/run.py --tracer ./tracer --backend both
```

Build an NP=1 tracer first. `run.py` owns native input-oracle checks, all four
manual input paths and CLI BFS plus three saved-edge replays. Its model uses two
choices at syscall 96 (`gettimeofday`) and startup boundary syscall 11. Each
session receives its exact generated `<session>/config.json`; results, hashes,
native outputs, transcripts and witnesses go beneath `--output` (default
`build/two_debits`). Results concern this finite fixture, not all native programs.

`zero_guard_benchmark.py` builds an owned variant with a large private anonymous
guard mapping, compares full/pages exploration and requires three terminal-edge
replays. It measures this synthetic case, not Redis performance:

```sh
python3 examples/two_debits/zero_guard_benchmark.py --tracer ./tracer --repeats 3 --size-mb 192
```

It requires NP=1; generated source, configuration, measurements and snapshots
go beneath `--output` (default `build/zero-guard-benchmark`).
