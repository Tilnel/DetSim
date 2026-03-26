#!/bin/bash
# Run tracer with optimized malloc settings to reduce 64MB mmap segments
#
# Problem: glibc malloc creates 64MB mmap segments when sbrk() fails due to fragmentation
# Solution: Force larger contiguous allocations and pre-allocate heap

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TRACER="${SCRIPT_DIR}/../tracer"

# Aggressive malloc tuning to prevent 64MB mmap segments
# These settings force malloc to use sbrk/heap even for large allocations
export MALLOC_ARENA_MAX=2                    # Strictly limit per-thread arenas
export MALLOC_MMAP_THRESHOLD_=2147483648     # 2GB - only use mmap for huge allocations
export MALLOC_TRIM_THRESHOLD_=524288         # 512KB - trim heap more frequently  
export MALLOC_TOP_PAD_=1073741824            # 1GB - pre-allocate heap space

echo "Starting tracer with aggressive malloc tuning:"
echo "  MALLOC_ARENA_MAX=$MALLOC_ARENA_MAX (limit per-thread arenas)"
echo "  MALLOC_MMAP_THRESHOLD_=$MALLOC_MMAP_THRESHOLD_ (2GB threshold, force heap usage)"
echo "  MALLOC_TRIM_THRESHOLD_=$MALLOC_TRIM_THRESHOLD_ (frequent trim)"
echo "  MALLOC_TOP_PAD_=$MALLOC_TOP_PAD_ (pre-allocate 1GB heap)"
echo ""

# Verify settings
echo "Current settings:"
echo "  Arena count check: $(grep -c "arena" /proc/$$/maps 2>/dev/null || echo 'N/A')"
echo ""

exec "$TRACER" "$@"
