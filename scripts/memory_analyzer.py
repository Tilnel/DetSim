#!/usr/bin/env python3
"""
Memory Analyzer for tracer
Analyzes /proc/PID/smaps and /proc/PID/maps to find memory hogs
"""

import sys
import os
import subprocess
import time
import json
from collections import defaultdict

def run_cmd(cmd):
    try:
        return subprocess.check_output(cmd, shell=True, text=True)
    except:
        return ""

def parse_smaps(pid):
    """Parse /proc/PID/smaps to find memory usage"""
    try:
        with open(f"/proc/{pid}/smaps", "r") as f:
            content = f.read()
    except:
        return {}
    
    # Group by mapping name
    mappings = defaultdict(lambda: {"rss": 0, "pss": 0, "size": 0, "count": 0})
    
    current_name = "[anonymous]"
    for line in content.split("\n"):
        if line and not line.startswith(" "):
            # Header line with mapping name
            parts = line.split()
            if len(parts) >= 6:
                name = parts[-1] if parts[-1].startswith("/") else "[anonymous]"
                current_name = name
        elif "Rss:" in line:
            rss = int(line.split()[1])  # KB
            mappings[current_name]["rss"] += rss
            mappings[current_name]["count"] += 1
        elif "Pss:" in line:
            pss = int(line.split()[1])  # KB
            mappings[current_name]["pss"] += pss
    
    return mappings

def get_heap_info(pid):
    """Get heap size from /proc/PID/maps"""
    try:
        with open(f"/proc/{pid}/maps", "r") as f:
            content = f.read()
    except:
        return 0, 0
    
    heap_size = 0
    anon_size = 0
    
    for line in content.split("\n"):
        if "[heap]" in line:
            parts = line.split()[0].split("-")
            if len(parts) == 2:
                start = int(parts[0], 16)
                end = int(parts[1], 16)
                heap_size = (end - start) // 1024  # KB
        elif "anon" in line or line.endswith("[anon]"):
            parts = line.split()[0].split("-")
            if len(parts) == 2:
                start = int(parts[0], 16)
                end = int(parts[1], 16)
                anon_size += (end - start) // 1024
    
    return heap_size, anon_size

def get_state_counts(pid):
    """Try to infer state counts from /proc/PID/fd or other heuristics"""
    try:
        # Count open files in memory/ and sstate/ directories
        fds = os.listdir(f"/proc/{pid}/fd")
        mem_files = 0
        sstate_files = 0
        
        for fd in fds:
            try:
                link = os.readlink(f"/proc/{pid}/fd/{fd}")
                if "/memory/" in link:
                    mem_files += 1
                elif "/sstate/" in link:
                    sstate_files += 1
            except:
                pass
        
        return mem_files, sstate_files
    except:
        return 0, 0

def analyze(pid, interval=5, count=3):
    """Analyze memory over time"""
    print(f"Analyzing PID {pid} every {interval}s for {count} iterations...")
    print("=" * 80)
    
    history = []
    
    for i in range(count):
        if i > 0:
            time.sleep(interval)
        
        # Get RSS
        try:
            with open(f"/proc/{pid}/status", "r") as f:
                status = f.read()
            for line in status.split("\n"):
                if line.startswith("VmRSS:"):
                    rss = int(line.split()[1]) // 1024  # MB
                    break
        except:
            rss = 0
        
        # Get smaps info
        mappings = parse_smaps(pid)
        heap, anon = get_heap_info(pid)
        mem_fd, sstate_fd = get_state_counts(pid)
        
        data = {
            "iteration": i,
            "rss_mb": rss,
            "heap_kb": heap,
            "anon_kb": anon,
            "mem_fds": mem_fd,
            "sstate_fds": sstate_fd,
            "top_mappings": sorted(mappings.items(), key=lambda x: x[1]["rss"], reverse=True)[:10]
        }
        history.append(data)
        
        print(f"\n[Iteration {i+1}/{count}] RSS: {rss} MB")
        print(f"  Heap: {heap//1024} MB, Anonymous: {anon//1024} MB")
        print(f"  Open mem files: {mem_fd}, sstate files: {sstate_fd}")
        
        print(f"  Top 5 Memory Mappings:")
        for name, info in data["top_mappings"][:5]:
            print(f"    {name[:50]:50} RSS: {info['rss']//1024:6} MB ({info['count']} regions)")
    
    # Calculate growth
    if len(history) >= 2:
        print("\n" + "=" * 80)
        print("GROWTH ANALYSIS")
        print("=" * 80)
        
        first = history[0]
        last = history[-1]
        dt = interval * (len(history) - 1)
        
        rss_growth = (last["rss_mb"] - first["rss_mb"]) / dt
        print(f"\nOverall Growth Rate:")
        print(f"  RSS: {rss_growth:.2f} MB/s ({rss_growth*3600:.0f} MB/hour)")
        
        # Analyze which mappings grew
        print(f"\nMapping Growth (top growing):")
        growth_list = []
        for name in set(list(first["top_mappings"].keys()) + [x[0] for x in last["top_mappings"]]):
            first_rss = next((info["rss"] for n, info in first["top_mappings"] if n == name), 0)
            last_rss = next((info["rss"] for n, info in last["top_mappings"] if n == name), 0)
            if last_rss > first_rss:
                growth_list.append((name, (last_rss - first_rss) // 1024))
        
        growth_list.sort(key=lambda x: x[1], reverse=True)
        for name, growth_mb in growth_list[:5]:
            print(f"  +{growth_mb:6} MB: {name[:50]}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <PID> [interval_seconds] [count]")
        print(f"Example: {sys.argv[0]} 12345 10 6")
        sys.exit(1)
    
    pid = sys.argv[1]
    interval = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    
    analyze(pid, interval, count)
