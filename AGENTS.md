# DetSim development contract

DetSim is a Linux x86-64 stateful model checker for native programs. Correctness
means exploring the intended transitions from faithfully restored snapshots.
A successful build or a running Redis example does not establish that property.

## Project context and credentials

- The authorized task is local development and correctness testing of this
  user-provided repository. Ptrace, memory capture and restore operate on test
  processes launched for the scenario in disposable working directories.
- Deliberate algorithm changes in owned build copies are regression fixtures:
  record the exact change and verify that DetSim detects the expected violation
  and reproduces it after restore. Keep upstream/vendor source intact.
- If credentials are needed, read them from task-specific environment variables
  or ignored local configuration. Keep their values out of prompts, command
  logs, test artifacts and commits; do not dump the environment.
- Keep routine reports focused on technical behavior and evidence, without
  speculative legal disclaimers. State the actual operations and limits clearly.

## Start here

1. Read [docs/development.md](docs/development.md) for commands and active paths.
2. Read [docs/correctness-backlog.md](docs/correctness-backlog.md) before touching
   scheduling, snapshots, state identity, or syscall emulation.
   Use [the state/atom contract assessment](docs/model-checking-contract.md) to
   distinguish runnable examples from a sound model and covered interleavings.
3. Check `git status --short`. Preserve unrelated work, ignored local experiments,
   submodule changes, and runtime snapshots.

`docs/index.md` explains the original design. Older optimization reports and
`REFACTORING_SUMMARY.md` are historical notes, not verification of current code.
Use source call sites and repeatable tests to settle contradictions.

## Work loop

- Choose one bounded defect or behavior. State the invariant and acceptance test.
- For cleanup, write the deletion/simplification plan before editing; lock behavior
  with regression tests first. Avoid bundling semantic fixes with restructuring.
- Prefer removal and existing utilities. No new dependencies without a request.
- Keep one implementation of each behavior. Trace callers before declaring code
  dead; moving code into a new directory does not establish that it is in use.
- Use direct work for small tasks; separate implementation and review for changes
  to build contracts or core semantics. Give delegated work explicit file ownership.
- Run `make check` and a production build when relevant. Report exact commands,
  changed files, simplifications, and unverified behavior. Do not call a build a test.
- Update the correctness backlog when evidence changes. Close an item only with a
  regression test and verification evidence, not with an implementation narrative.
- Commit completed, coherent changes after the relevant verification passes; the
  repository owner has requested autonomous commits. Keep unrelated changes and
  runtime artifacts out of commits. Push only when requested.

## Boundaries

- `src/` is product code; `tests/` is automated verification; `examples/` contains
  workloads/plugins and external projects. Do not treat their tests as DetSim tests.
- No workload-specific names or invariants in generic scheduling or persistence.
  Existing Raft coupling is debt, not a pattern to copy.
- Serialize semantic state. Host PIDs/TIDs, cache state, UI selection, statistics,
  and search bookkeeping need an explicit decision before entering state identity.
- Snapshot copying must isolate branches. Every new state field needs capture,
  copy, serialization, restore, and identity coverage.
- Unsupported emulation and restore failures must be explicit; do not manufacture
  success, silently drop state, or substitute live memory for unavailable snapshots.
- Optimizations need equivalent outcomes against a simple path, including cache
  misses/eviction and repeated restore. Performance measurements come afterward.

## Repository hygiene

- Check `git check-ignore -v <new-file>` if a file is missing from status. This repo
  has an inherited allowlist; do not broaden it to expose local artifacts wholesale.
- Keep generated/build/runtime files out of patches. Lexer/parser outputs are
  currently tracked; edit `.l`/`.y` inputs and review generated changes separately.
- Do not edit vendor trees or initialize/update submodules for unrelated work.
- Do not run `make clear`, delete snapshot stores, or change host ASLR as a routine
  check. Ptrace scenarios need a disposable working directory and process group.
- No bulk formatting, directory reshuffling, or new framework as a substitute for
  a failing test and a focused fix.

## Completion evidence

Distinguish: component tests; compilation; build-system regressions; ptrace scenario
tests; exploration-equivalence tests. Only claim the levels actually run.
Current gaps and next tasks live in `docs/correctness-backlog.md`.
