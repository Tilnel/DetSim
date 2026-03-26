#!/bin/bash
# Diagnose 64MB mmap allocations

PID=$1
if [ -z "$PID" ]; then
    echo "Usage: $0 <PID>"
    exit 1
fi

echo "=== Diagnosing 64MB allocations for PID $PID ==="
echo ""

# 1. Count exact sizes
echo "1. Exact size distribution of large rw-p mappings:"
cat /proc/$PID/maps | awk '
/^[0-9a-f]+-[0-9a-f]+ rw-p/ && $6 == "" {
    split($1, range, "-")
    start = strtonum("0x" range[1])
    end = strtonum("0x" range[2])
    size_kb = (end - start) / 1024
    
    # Count sizes around 64MB
    if (size_kb >= 64000 && size_kb <= 66000) {
        count[size_kb]++
    }
}
END {
    for (s in count) {
        printf "  %d KB: %d mappings\n", s, count[s]
    }
}' | sort -n

# 2. Check if these are heap segments (they should have [heap] label if they were main heap)
echo ""
echo "2. Checking if any are labeled as [heap]:"
cat /proc/$PID/maps | grep "\[heap\]"

# 3. Check malloc stats via gdb
echo ""
echo "3. Malloc stats (non-mmapped allocations):"
gdb -batch -p $PID -ex "call malloc_stats()" 2>/dev/null | grep -E "system bytes|in use bytes|mmap bytes" | head -10

# 4. Check if MALLOC_MMAP_THRESHOLD_ is effective
echo ""
echo "4. Environment variables from /proc/PID/environ:"
cat /proc/$PID/environ 2>/dev/null | tr '\0' '\n' | grep -E "MALLOC|MMAP" || echo "  (cannot read environ)"

# 5. Count total large anonymous mappings vs heap
echo ""
echo "5. Memory summary:"
HEAP_KB=$(cat /proc/$PID/maps | grep "\[heap\]" | awk '{split($1, r, "-"); print (strtonum("0x" r[2]) - strtonum("0x" r[1]))/1024}')
ANON_64MB=$(cat /proc/$PID/maps | awk '/rw-p/ && $6 == "" {split($1, r, "-"); s = (strtonum("0x" r[2]) - strtonum("0x" r[1]))/1024; if (s >= 64000 && s <= 66000) count++} END {print count}')
TOTAL_ANON=$(cat /proc/$PID/maps | awk '/rw-p/ && $6 == "" {split($1, r, "-"); total += (strtonum("0x" r[2]) - strtonum("0x" r[1]))} END {print total/1024/1024}')

echo "  [heap] size: ${HEAP_KB} KB"
echo "  ~64MB anonymous mappings: ${ANON_64MB}"
echo "  Total anonymous memory: ${TOTAL_ANON} MB"

echo ""
echo "=== Diagnosis ==="
if [ "$ANON_64MB" -gt 5 ]; then
    echo "LIKELY CAUSE: malloc falling back to mmap due to heap fragmentation"
    echo ""
    echo "When sbrk() cannot find contiguous space, malloc uses mmap() for large"
    echo "allocations, creating 64MB anonymous mappings (glibc's heap segment size)."
    echo ""
    echo "Try these solutions:"
    echo "1. Increase MALLOC_MMAP_THRESHOLD_ to force larger contiguous allocations:"
    echo "   export MALLOC_MMAP_THRESHOLD_=2147483648  # 2GB"
    echo ""
    echo "2. Pre-allocate a large heap at startup:"
    echo "   export MALLOC_TOP_PAD_=1073741824  # 1GB pad"
    echo ""
    echo "3. Use mallopt() in code to tune malloc behavior"
fi
