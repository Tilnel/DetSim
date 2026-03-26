# Malloc Tuning Guide for Tracer

## Problem: 64MB Memory Arenas

When running tracer, you may observe many 64MB anonymous mappings in `/proc/PID/maps`:

```
7f8b00000000-7f8b04000000 rw-p 00000000 00:00 0   <- 64MB arena
7f8b04000000-7f8b08000000 rw-p 00000000 00:00 0   <- another 64MB arena
...
```

These are **glibc malloc arenas**, not memory leaks.

### Why 64MB?

- glibc malloc creates per-thread arenas to reduce contention
- Default arena size on 64-bit Linux: **64MB**
- Default number of arenas: **8 × CPU cores** (can be hundreds!)

### Why Tracer Triggers This

1. **StateStore** creates multiple threads (IO workers + prefetch workers)
2. **512KB vector allocations** - larger than default 128KB mmap threshold
3. **Frequent alloc/free pattern** - causes arena memory to fragment

Result: RSS grows much larger than actual working set.

## Quick Fix

Run tracer with tuned malloc settings:

```bash
export MALLOC_ARENA_MAX=4                    # Limit arenas to 4
export MALLOC_MMAP_THRESHOLD_=1073741824     # 1GB - force heap usage
export MALLOC_TRIM_THRESHOLD_=1048576        # 1MB - trim frequently

./tracer -c raft.json
```

Or use the provided script:

```bash
./scripts/run_with_malloc_tuning.sh -c raft.json
```

## Environment Variables

| Variable | Default | Recommended | Effect |
|----------|---------|-------------|--------|
| `MALLOC_ARENA_MAX` | `8*cores` | `4` | Limit 64MB arenas |
| `MALLOC_MMAP_THRESHOLD_` | `128KB` | `1GB` | Force heap for StateStore vectors |
| `MALLOC_TRIM_THRESHOLD_` | `128KB` | `1MB` | Release memory more aggressively |

## Verification

Check if tuning is working:

```bash
# Run tracer
./tracer -c raft.json &
PID=$!

# Analyze arena count
./scripts/analyze_arenas.sh $PID

# Watch mmap growth
watch -n 1 'cat /proc/$PID/maps | grep -c "rw-p"'
```

Expected results:
- **Before tuning**: 20+ 64MB mappings
- **After tuning**: 4-8 64MB mappings (matching thread count)

## Implementation Details

### StateStore Thread Model

```
Main Thread
    ├── IO Worker Threads (2 by default)
    ├── Prefetch Threads (1 by default)
    └── Monitor Thread (1)
```

Each thread can trigger a new 64MB arena if:
- Arena limit not reached
- Current arena is locked by another thread
- Large allocation (>128KB default) requested

### Why MMAP_THRESHOLD Matters

StateStore uses 512KB vectors for state data:
- **Default (128KB)**: Goes to mmap → new arena per thread
- **Tuned (1GB)**: Goes to heap → reuses existing arena memory

## Permanent Fix (Makefile)

Add to your shell profile for permanent effect:

```bash
# ~/.bashrc or ~/.zshrc
export MALLOC_ARENA_MAX=4
export MALLOC_MMAP_THRESHOLD_=1073741824
export MALLOC_TRIM_THRESHOLD_=1048576
```

Or wrap tracer execution:

```bash
# Create alias
alias tracer='MALLOC_ARENA_MAX=4 MALLOC_MMAP_THRESHOLD_=1073741824 ./tracer'
```

## Performance Impact

| Metric | Default | Tuned | Notes |
|--------|---------|-------|-------|
| RSS | Unbounded | ~3GB | Bounded to working set |
| Alloc latency | Variable | Lower | No mmap syscall |
| Thread contention | Lower | Higher | Fewer arenas |

**Recommendation**: Use tuning for long-running BFS exploration.

## Related Tools

- `scripts/analyze_arenas.sh` - Count and analyze 64MB mappings
- `scripts/check_heap_mmap.sh` - Heap vs mmap breakdown
- `scripts/analyze_memory.sh` - Full memory analysis

## References

- [glibc malloc source](https://sourceware.org/git/?p=glibc.git;a=blob;f=malloc/malloc.c)
- [malloc arena documentation](https://www.gnu.org/software/libc/manual/html_node/Malloc-Tunable-Parameters.html)
