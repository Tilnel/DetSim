#!/usr/bin/env python3
"""
Analyze memory growth rate vs state growth rate
"""

import sys

if len(sys.argv) < 4:
    print(f"Usage: {sys.argv[0]} <rss_growth_mb_s> <state_growth_per_s> <runtime_seconds>")
    print(f"Example: {sys.argv[0]} 10 200 300")
    sys.exit(1)

rss_growth = float(sys.argv[1])  # MB/s
state_growth = float(sys.argv[2])  # states/s
runtime = int(sys.argv[3])  # seconds

memory_per_state = (rss_growth * 1024) / state_growth  # KB/state

print("="*60)
print("MEMORY GROWTH ANALYSIS")
print("="*60)
print(f"\nInput:")
print(f"  RSS Growth: {rss_growth} MB/s = {rss_growth*1024} KB/s")
print(f"  State Growth: {state_growth} states/s")
print(f"  Runtime: {runtime}s")

print(f"\nCalculated:")
print(f"  Memory per state: {memory_per_state:.1f} KB/state")

print(f"\nExpected memory per state:")
print(f"  sys_state struct (metadata): ~500 B")
print(f"  tracee_state × NP=3: ~3-5 KB")
print(f"  SockState/FSState: ~10-50 KB")
print(f"  StateStore L1 entry (raw): ~400 KB")
print(f"  StateStore L2 entry (compressed): ~50 KB")
print(f"  SysStateStore entry: ~200 B")

print(f"\nIf L1 cache working (512MB max, ~1300 entries):")
l1_contribution = min(512 * 1024 / 1300, 400)  # KB per state in L1
print(f"  L1 contribution per new state: ~{l1_contribution:.0f} KB")

print(f"\nIf L2 cache working (2GB max, ~40000 entries):")
l2_contribution = min(2 * 1024 * 1024 / 40000, 50)  # KB per state in L2
print(f"  L2 contribution per new state: ~{l2_contribution:.0f} KB")

print(f"\n*** DIAGNOSIS ***")
if memory_per_state > 100:
    print(f"  Memory/state ({memory_per_state:.0f}KB) is HIGH")
    print(f"  Possible causes:")
    print(f"    1. L1/L2 cache NOT evicting properly")
    print(f"    2. Rafts log entries accumulating")
    print(f"    3. UDP buffers accumulating (check SOCKS in monitor)")
    print(f"    4. Memory fragmentation")
elif memory_per_state > 50:
    print(f"  Memory/state ({memory_per_state:.0f}KB) is MODERATE")
    print(f"  L2 cache may be growing without bound")
else:
    print(f"  Memory/state ({memory_per_state:.0f}KB) is LOW")
    print(f"  This suggests the issue is NOT per-state overhead")
    print(f"  Check for:")
    print(f"    1. Memory leaks in unrelated code")
    print(f"    2. Global allocations")
    print(f"    3. Memory fragmentation")

print(f"\nProjected totals after {runtime}s:")
total_states = state_growth * runtime
total_memory_mb = rss_growth * runtime
print(f"  Total states: {total_states:.0f}")
print(f"  Total RSS growth: {total_memory_mb:.0f} MB")
print(f"  Final RSS (assuming 100MB base): {100 + total_memory_mb:.0f} MB")
