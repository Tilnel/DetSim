# Memory Tuning Guide

## Problem: Memory Fragmentation in StateStore

When running BFS exploration with high throughput (200 states/s), tracer's RSS grows at ~10MB/s even though StateStore L2 cache is bounded to 2GB.

### Root Cause: glibc malloc Fragmentation

`StateStore` uses a tiered cache:
1. **L1 (Hot)**: Raw uncompressed data, 512MB limit
2. **L2 (Warm)**: Compressed data, 2GB limit  
3. **Disk**: Persistent storage

When data moves from L1 → L2 → Disk:
- `raw_data.clear()` + `shrink_to_fit()` releases capacity
- `compressed_data` is moved to L2 cache
- When L2 is full, old entries are evicted

**The Problem**: `shrink_to_fit()` reduces vector capacity but glibc's malloc often **retains memory for reuse** rather than returning it to the OS. This causes RSS to grow much larger than actual working set.

### Symptoms

| Metric | Expected | Actual |
|--------|----------|--------|
| L2 Cache Size | 2GB | 2GB |
| RSS Growth | ~0 after warm-up | ~10MB/s |
| After 5 min | ~2.5GB | 3GB+ and OOM |

## Solution: malloc_trim

Added `malloc_trim()` call during L2 eviction to force glibc to return memory to OS.

### Configuration

In your JSON config file:

```json
{
  "StateStore": {
    "hot_cache_mb": 512,
    "warm_cache_mb": 2048,
    "enable_malloc_trim": true,
    "malloc_trim_threshold_mb": 64
  }
}
```

| Option | Default | Description |
|--------|---------|-------------|
| `enable_malloc_trim` | `false` | Enable periodic `malloc_trim(0)` calls |
| `malloc_trim_threshold_mb` | 64 | Evict this much from L2 before trimming |

### Trade-offs

**Pros:**
- Reduces RSS fragmentation significantly
- Prevents OOM on long-running BFS exploration

**Cons:**
- `malloc_trim()` has some overhead (~1-5ms per call)
- May hurt performance if called too frequently
- Only available on Linux (glibc)

## Monitoring

Use the provided script to check memory state:

```bash
# While tracer is running
./scripts/check_fragmentation.sh $(pidof tracer)
```

Look for:
- **High RSS vs Data**: Indicates fragmentation
- **Many anonymous mappings**: Fragmentation sign
- **StateStore disk usage**: Should grow steadily

## Benchmark Comparison

| Configuration | RSS Growth | Notes |
|--------------|------------|-------|
| `enable_malloc_trim=false` | ~10MB/s | Unbounded growth, OOM risk |
| `enable_malloc_trim=true` | ~0 after warm-up | Bounded to ~2.5-3GB |

## Implementation Details

The trim is triggered in `evict_l2_if_needed()` when accumulated evictions exceed threshold:

```cpp
// state_store.cpp
size_t total_evicted = 0;
{
    std::lock_guard<std::recursive_mutex> lock(warm_mutex_);
    // ... eviction logic ...
    total_evicted += freed;
}

// Outside lock - don't block other threads
if (should_trim) {
    malloc_trim(0);  // Release memory to OS
}
```

Design considerations:
1. **Outside lock**: `malloc_trim` can be slow, don't hold `warm_mutex_`
2. **Accumulated threshold**: Don't trim on every eviction (too expensive)
3. **Thread-safe**: Uses atomic counter for accumulation
