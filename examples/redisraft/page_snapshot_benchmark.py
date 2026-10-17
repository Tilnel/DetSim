"""Compare complete NP3 exploration with flat/page snapshots, sequentially."""
import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--tracer', type=Path, default=ROOT/'tracer')
p.add_argument('--before-tracer', type=Path, help='optional pre-branch NP3 binary')
p.add_argument('--plugin-dir', type=Path, default=ROOT/'build/release/np3/examples/redisraft')
p.add_argument('--output', type=Path, default=ROOT/'build/page-snapshot-benchmark')
p.add_argument('--repeats', type=int, default=3)
p.add_argument('--depth', type=int, default=12)
p.add_argument('--soft-dirty', action='store_true', help='also measure explicit unsafe experimental delta')
a = p.parse_args()
if a.repeats <= 0 or a.depth <= 0:
    p.error('repeats and depth must be positive')
a.output.mkdir(parents=True, exist_ok=True)
results = []
expected = None
modes = (['before'] if a.before_tracer else [])+['full', 'pages']
if a.soft_dirty:
    modes.append('pages-delta')
for repeat in range(a.repeats):
    for hot, warm in [(256, 512), (32, 64)]:
        for mode in (modes if repeat % 2 == 0 else list(reversed(modes))):
            env = os.environ.copy()
            for variable in ['DETSIM_SOFT_DIRTY_EXPERIMENT', 'DETSIM_SNAPSHOT_BACKEND', 'DETSIM_CAPTURE_MODE']:
                env.pop(variable, None)
            if mode.startswith('pages'):
                env['DETSIM_SNAPSHOT_BACKEND'] = 'pages'
            if mode == 'pages-delta':
                env['DETSIM_SOFT_DIRTY_EXPERIMENT'] = 'delta'
            tracer = a.before_tracer if mode == 'before' else a.tracer
            command = [sys.executable, str(Path(__file__).with_name('benchmark.py')),
                '--tracer', str(tracer.resolve()),
                '--choose', str(a.plugin_dir.resolve()/'choose.so'),
                '--check', str(a.plugin_dir.resolve()/'check.so'),
                '--seed-shim', str(a.plugin_dir.resolve()/'seed.so'),
                '--depth', str(a.depth), '--hot-mb', str(hot), '--warm-mb', str(warm),
                '--output', str((a.output/f'{mode}-{hot}').resolve())]
            completed = subprocess.run(command, env=env, capture_output=True, text=True)
            (a.output/f'{mode}-{hot}-{repeat}.log').write_text(completed.stdout+completed.stderr)
            if completed.returncode:
                raise RuntimeError('benchmark failed; see '+str(a.output))
            result = json.loads(completed.stdout)
            nodes = [sorted((k,v) for k,v in node.items() if k not in ['threads','node'])
                     for node in result['nodes']]
            signature = [result['dfs_states'], result['dfs_new_states'],
                         result['error_bound_after_manual'], nodes]
            if expected is None:
                expected = signature
            if signature != expected:
                raise RuntimeError('exploration work or protocol checkpoint changed')
            if mode.startswith('pages') and 'page_capture' not in result:
                raise RuntimeError('tracer did not report page snapshots')
            result['backend'] = mode
            results.append(result)
            (a.output/'results.json').write_text(json.dumps(results, indent=2))
            print(f'{mode} {hot}/{warm} MiB: {result["dfs_s"]:.3f}s', flush=True)

summary = {}
for hot in [32, 256]:
    summary[str(hot)] = {}
    for mode in modes:
        group = [r for r in results if r['hot_mb'] == hot and r['backend'] == mode]
        summary[str(hot)][mode] = {k: statistics.median(r[k] for r in group) for k in
            ['dfs_s', 'dfs_cpu_s', 'dfs_main_cpu_s', 'dfs_tracee_cpu_s',
             'save_ms', 'restore_ms', 'peak_rss_kib', 'store_bytes']}
(a.output/'summary.json').write_text(json.dumps(summary, indent=2))
print(json.dumps(summary, indent=2))
