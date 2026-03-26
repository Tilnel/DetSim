# 64MB Mmap Segment Issue

## Problem Description

When running tracer, `/proc/PID/maps` shows many anonymous mappings (`rw-p`) of approximately 64MB size:
- 65504 KB (64MB - 32KB)
- 65508 KB (64MB - 28KB) 
- 65512 KB (64MB - 24KB)
- Occasionally 65436 KB (64MB - 100KB)

These are **NOT** malloc arenas (already ruled out by `MALLOC_ARENA_MAX=4`).

## Root Cause

**Heap fragmentation causing `sbrk()` to fail.**

When glibc malloc cannot extend the heap via `sbrk()` (due to fragmentation or limits), it falls back to `mmap()` to allocate new **heap segments**. On 64-bit systems, these segments are typically 64MB minus a small metadata overhead (hence ~65504KB).

### Why This Happens

1. Tracer allocates many 512KB state buffers
2. When L1 cache is full, these buffers are freed
3. Heap becomes fragmented with 512KB holes
4. When malloc needs a large contiguous block (>128KB default), `sbrk()` fails to find 64MB contiguous space
5. Malloc falls back to `mmap()`, creating a new 64MB heap segment
6. Over time, many 64MB segments accumulate, causing RSS bloat

## Why MALLOC_MMAP_THRESHOLD_=1G Doesn't Help

The `MALLOC_MMAP_THRESHOLD_` controls when malloc uses `mmap()` for **individual allocations**. However, when `sbrk()` fails, malloc **must** use `mmap()` for heap segments regardless of this threshold.

## Solutions

### Solution 1: Pre-allocate Heap Space (Recommended)

```bash
export MALLOC_TOP_PAD_=1073741824    # 1GB - pre-allocate heap at startup
export MALLOC_MMAP_THRESHOLD_=2147483648  # 2GB - force individual allocs to heap
./tracer -c raft.json
```

`MALLOC_TOP_PAD_` tells malloc to expand the heap by at least 1GB when it needs to grow, preventing frequent `sbrk()` failures.

### Solution 2: Use jemalloc (Alternative Allocator)

```bash
sudo apt-get install libjemalloc2
export LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2
./tracer -c raft.json
```

jemalloc handles fragmentation better than glibc malloc for this workload.

### Solution 3: Limit Concurrent Allocations

Reduce the number of concurrent state buffers:
- Decrease IO queue size
- Decrease prefetch window
- Limit BFS branching factor

### Solution 4: Custom Memory Pool (Code Change)

Implement a memory pool for 512KB state buffers to reuse memory instead of alloc/free:

```cpp
class StateBufferPool {
    std::vector<std::vector<uint8_t>> free_buffers_;
    static constexpr size_t BUFFER_SIZE = 512 * 1024;
public:
    std::vector<uint8_t> acquire() {
        if (!free_buffers_.empty()) {
            auto buf = std::move(free_buffers_.back());
            free_buffers_.pop_back();
            return buf;
        }
        return std::vector<uint8_t>(BUFFER_SIZE);
    }
    void release(std::vector<uint8_t>&& buf) {
        if (buf.capacity() == BUFFER_SIZE) {
            free_buffers_.push_back(std::move(buf));
        }
    }
};
```

## Verification

Run the diagnosis script while tracer is running:

```bash
./scripts/diagnose_64mb.sh $(pidof tracer)
```

Expected output after fix:
- Fewer than 5 x 64MB mappings
- [heap] size grows to accommodate L1+L2 cache
- Total anonymous memory stabilizes at ~3GB

## Implementation Status

- [x] Diagnosis scripts created
- [x] Malloc tuning script updated with `MALLOC_TOP_PAD_`
- [ ] Memory pool implementation (optional, if tuning insufficient)

## References

- [Glibc malloc implementation](https://sourceware.org/git/?p=glibc.git;a=blob;f=malloc/malloc.c)
- [jemalloc vs glibc malloc](http://jemalloc.net/)
- [Linux malloc tunables](https://www.gnu.org/software/libc/manual/html_node/Malloc-Tunable-Parameters.html)
