#!/bin/bash
# Check if memory growth is due to fragmentation

PID=$1
if [ -z "$PID" ]; then
    echo "Usage: $0 <PID>"
    exit 1
fi

echo "Analyzing memory fragmentation for PID $PID..."
echo ""

# Get heap info from /proc/PID/smaps_rollup (if available)
if [ -f "/proc/$PID/smaps_rollup" ]; then
    echo "=== smaps_rollup ==="
    cat /proc/$PID/smaps_rollup | grep -E "Rss|Pss|Anonymous"
fi

# Calculate RSS vs actual data sizes
echo ""
echo "=== Memory Breakdown ==="
RSS=$(awk '/VmRSS/ {print $2}' /proc/$PID/status)
DATA=$(awk '/VmData/ {print $2}' /proc/$PID/status)
STK=$(awk '/VmStk/ {print $2}' /proc/$PID/status)
EXE=$(awk '/VmExe/ {print $2}' /proc/$PID/status)
LIB=$(awk '/VmLib/ {print $2}' /proc/$PID/status)

echo "  RSS:     ${RSS} kB"
echo "  Data:    ${DATA} kB"
echo "  Stack:   ${STK} kB"
echo "  Code:    ${EXE} kB"
echo "  Libs:    ${LIB} kB"

# Check malloc info
echo ""
echo "=== malloc_info ==="
gdb -batch -p $PID -ex "call malloc_info(0, stdout)" 2>/dev/null | head -50 || echo "(gdb not available or no debug symbols)"

# Count anonymous mappings (potential fragmentation)
echo ""
echo "=== Anonymous Mappings ==="
cat /proc/$PID/maps | grep -E "\[heap\]|\[anon\]" | wc -l
echo "(High count indicates fragmentation)"

# Check StateStore files (disk usage)
echo ""
echo "=== StateStore Disk Usage ==="
if [ -d memory ]; then
    echo "  memory/: $(du -sh memory/ 2>/dev/null | cut -f1) ($(ls memory/*.mem.zstd 2>/dev/null | wc -l) files)"
fi
if [ -d sstate ]; then
    echo "  sstate/: $(du -sh sstate/ 2>/dev/null | cut -f1)"
    if [ -f sstate/packed.dat ]; then
        echo "    packed.dat: $(ls -lh sstate/packed.dat | awk '{print $5}')"
    fi
fi
