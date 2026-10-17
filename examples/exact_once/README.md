# Runtime producer/consumer cases

The Rust fixture uses `std::thread`, an `mpsc` channel and `join`. The CPython
default fixture uses `threading.Thread`, `threading.Condition`, a guarded list and
`join`. The separate `--python-target queue` fixture uses `queue.Queue`, including
its optional `_queue` extension. These are independent targets; success for
Condition does not admit Queue or its extension mappings. The main thread produces
task IDs 0 and 1; the worker consumes until the channel closes, the guarded completion flag is
set with no pending tasks, or the Queue sentinel arrives. Both runtimes retain their normal GC, preemption and thread settings.
CPython keeps the existing admission driver's `-I -S` invocation.

`healthy` delivers both tasks. `omit` skips task 1; `duplicate` sends task 1 twice.
All variants finish natively and report what the consumer actually received;
they do not assert their own expected answer. The independent checker requires
exact output `EXACT_ONCE 0 1`, at least two saved logical thread slots, and zero
process exit. Missing or duplicate task output is a property violation. Runtime
futex waits are not classified as deadlock.

From the repository root, with an already-built NP=1 tracer:

```sh
CC=/usr/bin/gcc CXX=/usr/bin/g++ python3 examples/exact_once/run.py \
  --tracer ./tracer --backend both --languages rust,python \
  --variants healthy,omit,duplicate --timeout 600 --depth 1500 --memory-mb 4096
```

Each model run gets a fresh tracer and snapshot store. Reports retain actual
compiler/interpreter versions, commands, hashes, captured rootfs manifest,
configuration, native output, search status and endpoint/counterexample replay.
A checked completion or counterexample must replay its final edge three times
with the same local state identity. Unsupported calls, timeout and depth-bound
results remain non-green. Results concern these bounded syscall paths under the
current actor policy; they do not establish exhaustive runtime interleavings.

The Queue target has exposed explicit unsupported SIGBUS-backed file mapping
during `_queue` loading. Its failed model report must remain non-green; choosing
the Condition target does not resolve that generic file EOF-tail mapping gap.

Go targets are explicit opt-ins, leaving the default Rust/CPython matrix intact:

```sh
python3 examples/exact_once/run.py --languages go-gccgo --backend pages --timeout 180
python3 examples/exact_once/run.py --languages go-gccgo --backend full --timeout 600
python3 examples/exact_once/run.py --languages go-gc --backend pages --timeout 180
```

Both Go targets use the same real goroutine, unbuffered task channel, channel
close and result-channel join. `go-gccgo` builds with `/usr/bin/go` and requires
its version to identify gccgo. `go-gc` builds with the isolated official compiler
at `build/toolchains/go1.23.12/go/bin/go` and rejects gccgo identity. Build commands explicitly select `-compiler=gccgo` or `-compiler=gc`, overriding
a conflicting `GOFLAGS` compiler selection. Neither changes GC, preemption or
scheduler settings. Compiler versions, exact builds,
primitives and runtime profiles are recorded per row. gccgo retains startup
syscall 11; the static standard gc executable uses startup syscall 9. These
compiler/runtime targets have separate matrix keys and admission results.

`run.py` owns each compiler/runtime profile, plugin list, rootfs input manifest,
startup boundary and bounded search settings. `exact_once_checker.cpp` is loaded
by DetSim. Each controlled session writes its exact generated `config.json` in
that session's output directory. Generated binaries, reports and snapshots belong
under `--output`, not in this example's source directory.
