"""Summarize DFS-only perf script stacks; CPU samples are not wall-clock shares.

Input stacks: perf script -i perf.data -F comm,pid,tid,time,event,ip,sym,dso
Unknown/truncated stacks remain in 'main other'; categories never overlap.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('result', type=Path)
p.add_argument('stacks', type=Path)
a = p.parse_args()
result = json.loads(a.result.read_text())
begin, end = result['perf_window_monotonic_s']
counts, main, capture = Counter(), Counter(), Counter()
kernel = 0


def consume(block):
    global kernel
    match = re.match(r'\S+\s+(\d+)/(\d+)\s+([\d.]+):', block)
    if not match or not begin <= float(match[3]) <= end:
        return
    is_main = match[1] == match[2]
    if not is_main:
        category = 'worker' if 'StateStore::io_worker' in block else 'monitor/other threads'
    elif 'show_syscall_history' in block:
        category = 'history'
    elif 'check_state()' in block or ' check (' in block:
        category = 'check'
    elif 'save_full_state_to_state_store' in block:
        category = 'capture'
    elif any(s in block for s in ('recover_running_state', 'recover_mem_reg_snapshot',
                                  'restore_memory_mappings')):
        category = 'restore'
    elif 'sys_state::sys_state(unsigned long)' in block:
        category = 'DFS reload'
    elif 'exec_once' in block or 'ExecutionEngine::' in block:
        category = 'execute'
    else:
        category = 'main other'
    counts[category] += 1
    if is_main:
        main[category] += 1
    frames = block.splitlines()[1:]
    if frames and '[kernel.kallsyms]' in frames[0]:
        kernel += 1
    if category == 'capture':
        if 'process_vm_readv' in block:
            subcategory = 'cross-process read'
        elif 'StateStore::save(' in block:
            subcategory = 'dedup/hash/copy'
        elif '__memset_' in block:
            subcategory = 'zero initialize'
        elif any(s in block for s in ('get_maps_item', 'open_cfile', '_IO_fgets')):
            subcategory = 'maps read/parse'
        else:
            subcategory = 'other capture'
        capture[subcategory] += 1


block = []
with a.stacks.open() as stream:
    for line in stream:
        if line.strip():
            block.append(line)
        elif block:
            consume(''.join(block))
            block.clear()
if block:
    consume(''.join(block))
total = sum(counts.values())
main_total = sum(main.values())
if not total or not main_total:
    p.error('no main-thread samples in the recorded DFS window')
print(json.dumps({
    'samples': total,
    'main_samples': main_total,
    'kernel_percent': 100 * kernel / total,
    'all_cpu_percent': {k: 100 * v / total for k, v in counts.items()},
    'main_cpu_percent': {k: 100 * v / main_total for k, v in main.items()},
    'capture_cpu_breakdown_percent_of_main': {
        k: 100 * v / main_total for k, v in capture.items()},
}, indent=2))
