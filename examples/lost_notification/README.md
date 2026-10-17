# Lost notification

`lost_notification.c` is a pthread condition-variable driver. `INJECT_BUG`
introduces a lost notification; `deadlock_checker.cpp` checks the fixture's
closed-world untimed wait condition. The explicit shared marker plugin is
`../runtime_admission/admission_checker.cpp`, which checks the `RUNTIME_OK` output.
A native timeout is recorded as expected behavior for the mutant, not itself
accepted as a proof of deadlock.

```sh
python3 examples/lost_notification/run.py --tracer ./tracer --backend both --depth 100
```

An NP=1 tracer is required. `run.py` owns compilation, native oracle checks and
bounded CLI DFS with saved-edge replay. The startup boundary is syscall 11;
there are no workload clock choices. Exact generated configurations live at
`<session>/config.json`; reports, transcripts and replay evidence live under
`--output` (default `build/lost_notification`). Depth cutoffs, unsupported calls
and timeouts remain non-green; the claim is limited to this fixture/profile.
