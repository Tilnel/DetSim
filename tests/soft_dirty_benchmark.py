"""Sequential alternating full/delta Raft exploration experiment (NP=3)."""
import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--tracer', type=Path, default=ROOT/'tracer')
p.add_argument('--plugin-dir', type=Path, default=ROOT/'build/release/np3/tests')
p.add_argument('--output', type=Path, default=ROOT/'build/soft-dirty-benchmark')
p.add_argument('--repeats', type=int, default=3)
p.add_argument('--depth', type=int, default=12)
a = p.parse_args()
if a.repeats <= 0 or a.depth <= 0:
    p.error('repeats and depth must be positive')
a.output.mkdir(parents=True, exist_ok=True)
results = []
expected = None
for repeat in range(a.repeats):
    for hot, warm in [(256, 512), (32, 64)]:
        for mode in (['full', 'delta'] if repeat % 2 == 0 else ['delta', 'full']):
            env = os.environ.copy()
            env.pop('DETSIM_SOFT_DIRTY_EXPERIMENT', None)
            env.pop('DETSIM_CAPTURE_MODE', None)
            if mode == 'delta':
                env['DETSIM_SOFT_DIRTY_EXPERIMENT'] = mode
            command = [sys.executable, str(ROOT/'tests/redisraft_benchmark.py'),
                '--tracer', str(a.tracer.resolve()),
                '--choose', str(a.plugin_dir.resolve()/'redisraft_choose.so'),
                '--check', str(a.plugin_dir.resolve()/'redisraft_check.so'),
                '--seed-shim', str(a.plugin_dir.resolve()/'redisraft_seed.so'),
                '--depth', str(a.depth), '--hot-mb', str(hot), '--warm-mb', str(warm),
                '--output', str((a.output/f'{mode}-{hot}').resolve())]
            completed = subprocess.run(command, env=env, capture_output=True, text=True)
            (a.output/f'{mode}-{hot}-{repeat}.log').write_text(completed.stdout+completed.stderr)
            if completed.returncode:
                raise RuntimeError('benchmark failed; see '+str(a.output))
            result = json.loads(completed.stdout)
            signature = [result['dfs_states'], result['dfs_new_states'],
                result['storage_stats']['saves'], result['storage_stats']['new_snapshots'],
                result['error_bound_after_manual'], result['leader_count']]
            if expected is None:
                expected = signature
            if signature != expected:
                raise RuntimeError('workload counts differ: '+str(signature)+' vs '+str(expected))
            if mode == 'delta' and 'soft_dirty' not in result:
                raise RuntimeError('tracer did not report the soft-dirty experiment')
            result['capture_mode'] = mode
            results.append(result)
            (a.output/'results.json').write_text(json.dumps(results, indent=2))
            print(f'{mode} {hot}/{warm} MiB: {result["dfs_s"]:.3f}s', flush=True)

summary = {}
for hot in [32, 256]:
    summary[str(hot)] = {}
    for mode in ['full', 'delta']:
        group = [r for r in results if r['hot_mb'] == hot and r['capture_mode'] == mode]
        medians = {k: statistics.median(r[k] for r in group) for k in
            ['dfs_s', 'dfs_cpu_s', 'dfs_main_cpu_s', 'dfs_tracee_cpu_s',
             'save_ms', 'restore_ms', 'peak_rss_kib']}
        if mode == 'delta':
            medians['soft_dirty'] = {k: statistics.median(r['soft_dirty'][k] for r in group)
                                    for k in group[0]['soft_dirty']}
        summary[str(hot)][mode] = medians
(a.output/'summary.json').write_text(json.dumps(summary, indent=2))
print(json.dumps(summary, indent=2))
