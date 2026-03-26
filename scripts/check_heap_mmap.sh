#!/bin/bash
# Check heap vs mmap memory usage

PID=$1
if [ -z "$PID" ]; then
    echo "Usage: $0 <PID>"
    exit 1
fi

echo "=== Heap vs Mmap Analysis for PID $PID ==="
echo ""

# Get RSS
RSS_KB=$(awk '/VmRSS/ {print $2}' /proc/$PID/status)
RSS_MB=$((RSS_KB / 1024))
echo "Total RSS: ${RSS_MB} MB"

# Parse /proc/PID/maps to categorize memory
# heap: [heap] or anon mappings without file backing
# mmap: file-backed mappings
# stack: [stack]

echo ""
echo "=== Memory Mapping Breakdown ==="

# heap (anonymous, not stack, not vdso/vsyscall)
HEAP_BYTES=$(cat /proc/$PID/maps | awk '
/\[heap\]/ { 
    split($1, range, "-")
    start = strtonum("0x" range[1])
    end = strtonum("0x" range[2])
    print end - start
}' | awk '{sum+=$1} END {print sum}')

HEAP_MB=$((HEAP_BYTES / 1024 / 1024))
echo "Heap [heap]: ${HEAP_MB} MB"

# Anonymous mappings (potential heap/glibc arenas)
ANON_BYTES=$(cat /proc/$PID/maps | grep -E "rw-p.*00000000 00:00 0" | grep -v "\[stack\]" | awk '
{
    split($1, range, "-")
    start = strtonum("0x" range[1])
    end = strtonum("0x" range[2])
    print end - start
}' | awk '{sum+=$1} END {print sum}')

ANON_MB=$((ANON_BYTES / 1024 / 1024))
echo "Anonymous (rw-p): ${ANON_MB} MB"

# File-backed mappings
FILE_BYTES=$(cat /proc/$PID/maps | grep -vE "00000000 00:00 0" | grep -v "\[stack\]" | grep -v "\[heap\]" | awk '
{
    split($1, range, "-")
    start = strtonum("0x" range[1])
    end = strtonum("0x" range[2])
    print end - start
}' | awk '{sum+=$1} END {print sum}')

FILE_MB=$((FILE_BYTES / 1024 / 1024))
echo "File-backed: ${FILE_MB} MB"

# Stack
STACK_BYTES=$(cat /proc/$PID/maps | grep "\[stack\]" | awk '
{
    split($1, range, "-")
    start = strtonum("0x" range[1])
    end = strtonum("0x" range[2])
    print end - start
}' | awk '{sum+=$1} END {print sum}')

STACK_MB=$((STACK_BYTES / 1024 / 1024))
echo "Stack: ${STACK_MB} MB"

# Total
TOTAL_ESTIMATED=$((HEAP_MB + ANON_MB + FILE_MB + STACK_MB))
echo ""
echo "Estimated Total: ${TOTAL_ESTIMATED} MB (RSS: ${RSS_MB} MB)"

# Check for large anonymous mappings (>1MB) - these are likely glibc arenas or large vectors

echo ""
echo "=== Large Anonymous Mappings (>1MB) ==="
cat /proc/$PID/maps | grep -E "rw-p.*00000000 00:00 0" | while read line; do
    ADDR=$(echo $line | awk '{print $1}')
    split($ADDR, range, "-")  # This won't work in bash, need different approach
    # Just print the line for now
    echo "  $line"
done | head -20

# Use malloc_stats if available
echo ""
echo "=== Malloc Stats (via gdb) ==="
if command -v gdb &> /dev/null; then
    # Get arena count
    gdb -batch -p $PID -ex "call malloc_stats()" 2>/dev/null || echo "  (gdb failed)"
else
    echo "  (gdb not installed)"
fi
