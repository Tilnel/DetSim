#!/bin/bash
# Analyze tracer memory to find large allocations

PID=$1
if [ -z "$PID" ]; then
    echo "Usage: $0 <PID>"
    exit 1
fi

echo "=== Memory Analysis for PID $PID ==="
echo ""

# 1. Get RSS and heap
echo "1. Basic Memory Stats:"
cat /proc/$PID/status | grep -E "VmRSS|VmData|VmSize|Threads"
echo ""

# 2. Check /proc/PID/maps for large anonymous mappings
echo "2. Large Anonymous Mappings (>4KB):"
cat /proc/$PID/maps | grep -v "r--p" | grep -v "r-xp" | awk '
BEGIN { total = 0 }
{
    # Parse address range like "7f8b0000-7f8c0000"
    split($1, range, "-")
    start = strtonum("0x" range[1])
    end = strtonum("0x" range[2])
    size = end - start
    
    if (size > 4*1024) {
        printf "  %s size=%.2fKB %s\n", $1, size/1024, $6
        total += size
    }
}
END {
    printf "  Total large anon mappings: %.2fMB\n", total/1024/1024
}'
echo ""

# 3. Check for heap fragmentation
echo "3. Heap Info (if available):"
if [ -f /proc/$PID/smaps_rollup ]; then
    cat /proc/$PID/smaps_rollup | grep -E "Rss|Private_Dirty|Anonymous"
fi
echo ""

# 4. Count number of mappings (fragmentation indicator)
echo "4. Fragmentation Metrics:"
MAPS_COUNT=$(cat /proc/$PID/maps | wc -l)
HEAP_COUNT=$(cat /proc/$PID/maps | grep "\[heap\]" | wc -l)
ANON_COUNT=$(cat /proc/$PID/maps | grep -E "\[stack\]|\[anon\]" | wc -l)
echo "  Total mappings: $MAPS_COUNT"
echo "  Heap segments: $HEAP_COUNT"
echo "  Anonymous segments: $ANON_COUNT"
echo ""

# 6. Check open files (StateStore disk files)
echo "6. StateStore Disk Files:"
ls -la /proc/$PID/fd/ 2>/dev/null | grep -E "memory/|sstate/" | wc -l
echo "  Open StateStore files: $(ls -la /proc/$PID/fd/ 2>/dev/null | grep -E 'memory/|sstate/' | wc -l)"

