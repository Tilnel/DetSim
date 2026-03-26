#!/bin/bash
# Watch tracer memory in real-time

PID=$(pgrep -f "./tracer -c" | head -1)
if [ -z "$PID" ]; then
    echo "Tracer not running"
    exit 1
fi

echo "Watching PID $PID..."
echo "Time, RSS(MB), Heap(MB), L1(MB), L2(MB), Tree, Set, DataFile(MB)"

while true; do
    RSS=$(awk '/VmRSS/ {print int($2/1024)}' /proc/$PID/status)
    HEAP=$(cat /proc/$PID/maps | grep '\[heap\]' | awk -F'-' '{print strtonum("0x"$2) - strtonum("0x"$1)}' | awk '{sum+=$1} END {print int(sum/1024/1024)}')
    
    # Count state files
    MEM_FILES=$(ls memory/*.mem.zstd 2>/dev/null | wc -l)
    DATA_SIZE=$(du -sm memory/ 2>/dev/null | cut -f1)
    
    echo "$(date +%H:%M:%S), ${RSS:-0}, ${HEAP:-0}, -, -, -, -, ${DATA_SIZE:-0}"
    
    sleep 5
done
