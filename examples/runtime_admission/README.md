# Runtime admission

`probes/` contains small C, C++, Rust, Go, Python and Java programs.
`admission_checker.cpp` checks the common `RUNTIME_OK` output. This case probes
runtime admission, controlled stepping, saved-state restore and bounded search;
it does not decode Python or JVM object semantics.

```sh
python3 examples/runtime_admission/run.py --tracer ./tracer --languages c,cpp,rust,python --backend both
```

An NP=1 tracer and each requested compiler/runtime must already exist. `run.py`
owns target build commands, explicit rootfs inputs and configuration policy.
`--startup-syscall` defaults to 11; Python uses its existing `-I -S` invocation,
and Java uses the explicitly bounded heap/code-cache arguments shown in the
runner. Unsupported runtimes, timeouts and incomplete bounded searches are not
passes. Every session records its exact `<session>/config.json`; rootfs manifests,
reports, transcripts and replay evidence go beneath `--output` (default
`build/runtime_admission`). No exploration-time discovery imports arbitrary host
files. The marker plugin is also explicitly reused by `lost_notification`.
