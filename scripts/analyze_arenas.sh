#!/bin/bash
# Analyze glibc malloc arenas in tracer

PID=$1
if [ -z "$PID" ]; then
    echo "Usage: $0 <PID>"
    exit 1
fi

echo "=== Arena Analysis for PID $PID ==="
echo ""

# Count 64MB mappings (likely arenas)
echo "1. Counting ~64MB anonymous mappings (likely arenas):"
cat /proc/$PID/maps | awk '
/^[0-9a-f]+-[0-9a-f]+ rw-p/ {
    split($1, range, "-")
    start = strtonum("0x" range[1])
    end = strtonum("0x" range[2])
    size = end - start
    
    # Count mappings between 60MB and 70MB
    if (size >= 60000000 && size <= 70000000 && $6 == "") {
        count++
        total += size
    }
}
END {
    printf "  Found %d mappings (~64MB each), total: %.1fMB\n", count, total/1024/1024
}'

# Show actual arena sizes distribution
echo ""
echo "2. Anonymous rw-p mapping size distribution:"
cat /proc/$PID/maps | awk '
/^[0-9a-f]+-[0-9a-f]+ rw-p/ {
    split($1, range, "-")
    start = strtonum("0x" range[1])
    end = strtonum("0x" range[2])
    size = end - start
    
    if ($6 == "") {  # No file backing
        if (size < 65536) small++
        else if (size < 1048576) medium++
        else if (size < 67108864) large++
        else huge++
    }
}
END {
    printf "  <64KB:    %d\n", small
    printf "  64KB-1MB:  %d\n", medium
    printf "  1MB-64MB:  %d\n", large
    printf "  >=64MB:    %d\n", huge
}'

echo ""
echo "3. Thread count (should correlate with arenas):"
cat /proc/$PID/status | grep -E "Threads|Pid"

echo ""
echo "4. Malloc environment (current process):"
echo "  MALLOC_ARENA_MAX=${MALLOC_ARENA_MAX:-'(not set, default=8*cores)' }"
echo "  MALLOC_MMAP_THRESHOLD_=${MALLOC_MMAP_THRESHOLD_:-'(not set, default=128KB)' }"
echo "  MALLOC_TRIM_THRESHOLD_=${MALLOC_TRIM_THRESHOLD_:-'(not set, default=128KB)' }"

echo ""
echo "5. Current arena count via gdb (if available):"
if command -v gdb &> /dev/null; then
    # Try to get arena count from malloc_stats
    gdb -batch -p $PID -ex "call malloc_stats()" 2>/dev/null | grep -E "Arena|arena|thread" | head -10 || echo "  (gdb failed)"
else
    echo "  (gdb not installed)"
fi

echo ""
echo "=== Diagnosis ==="
echo "If you see many 64MB mappings (> thread count), possible causes:"
echo "1. Threads being created/destroyed frequently"
echo "2. Memory fragmentation causing arena growth"
echo "3. Large allocations (>128KB default) going to mmap instead of heap"
echo ""
echo "Recommended fixes:"
echo "  export MALLOC_ARENA_MAX=4          # Limit arenas to 4"
echo "  export MALLOC_MMAP_THRESHOLD_=1g   # Only mmap for >1GB (force heap)"
