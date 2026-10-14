# Refactoring Summary

> Historical implementation notes. The build claims below are not a current
> verification baseline. See [development.md](docs/development.md) and the
> [correctness backlog](docs/correctness-backlog.md); the two scheduler paths
> have not been consolidated or verified as equivalent.

## Iteration 7: Final State

### Code Metrics

| File | Lines | Change |
|------|-------|--------|
| state_transition.cpp | 484 | -720 (from ~1204) |
| ThreadManager.cpp | 395 | New |
| SignalHandler.cpp | 235 | New |
| ExecutionEngine.cpp | 321 | New |
| SyscallDispatcher.cpp | 417 | New |
| handlers.cpp | 487 | New |
| **Total new code** | **1855** | |

### Functions Removed

- `on_syscall_enter()` - ~200 lines → SyscallDispatcher
- `on_syscall_exit()` - ~360 lines → handlers::
- `do_one_syscall()` - ~95 lines → ExecutionEngine
- `handle_thread_exit()` - ~60 lines → ThreadManager
- `do_nosys()` - ~10 lines → removed (dead code)

### New Architecture

```
src/core/engine/
  ├── thread_manager.{h,cpp}    - Thread lifecycle & scheduling
  ├── signal_handler.{h,cpp}    - Signal classification  
  └── exec_engine.{h,cpp}       - Integrated execution

src/core/syscall/
  ├── dispatcher.{h,cpp}        - Syscall entry/exit routing
  └── handlers.{h,cpp}          - Category-specific handlers

src/subsys/
  ├── fs/   - Filesystem (fsstate, fd_manager)
  ├── net/  - Network (sockstate, emu, serialize)
  ├── sync/ - Futex (futexstate)
  └── time/ - Time emulation
```

### Current exec_once()

```cpp
int exec_once(const sys_state &s, syscall_info &info) {
  ExecutionEngine* engine = exec::get_engine();
  ExecResult result = engine->execute_step(s, info);
  return result.checkpoint_decision;
}
```

### Build Status

```
$ make NP=1
# Build succeeds
$ ls -la tracer
# 1.7M binary
```
