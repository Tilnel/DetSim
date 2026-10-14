"""Bounded benchmark for the repository's three-node Redis Raft-library driver."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import select
import signal
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--tracer', type=Path, default=ROOT/'tracer')
p.add_argument('--binary', type=Path, default=ROOT/'examples/redisraft/raft/raft')
p.add_argument('--choose', type=Path, default=ROOT/'build/redisraft-benchmark/choose.so')
p.add_argument('--seed-shim', type=Path, help='benchmark-only srand interposer')
p.add_argument('--seed', type=int, default=1)
p.add_argument('--check', type=Path, help='optional existing Raft checker plugin')
p.add_argument('--output', type=Path, default=ROOT/'build/redisraft-benchmark')
p.add_argument('--rounds', type=int, default=100)
p.add_argument('--depth', type=int, default=5)
p.add_argument('--hot-mb', type=int, default=32)
p.add_argument('--warm-mb', type=int, default=64)
p.add_argument('--timeout', type=float, default=60)
p.add_argument('--perf', type=Path, help='perf executable: sample only DFS (all tracer threads)')
p.add_argument('--perf-sudo', action='store_true', help='run perf with sudo -n; do not change sysctl')
a = p.parse_args()
if a.rounds <= 0 or a.depth < 0 or a.hot_mb < 0 or a.warm_mb < 0 or a.timeout <= 0:
    p.error("invalid rounds, depth, cache size or timeout")
a.output.mkdir(parents=True, exist_ok=True)
work = Path(tempfile.mkdtemp(prefix='run-', dir=a.output.resolve()))
result = {'passed': False, 'workdir':str(work), 'rounds':a.rounds, 'dfs_depth':a.depth,
          'hot_mb':a.hot_mb, 'warm_mb':a.warm_mb, 'checker':str(a.check) if a.check else None, 'steps':0, 'peak_rss_kib':0,
          'search_seed':a.seed if a.seed_shim else None,
          'binary_sha256':hashlib.sha256(a.binary.read_bytes()).hexdigest()}
config = {'Nodes':3,'Loglevel':4,'Tracee':[[str(a.binary.resolve()),str(i)] for i in range(3)],
          'Addr':['192.168.0.1','192.168.0.2','192.168.0.3'], 'WorkingDir':'/',
          'ChoosePoint':[{'syscall':96,'choose':2}], 'ChooseFunc':str(a.choose.resolve()),
          'StateStore':{'hot_cache_mb':a.hot_mb,'warm_cache_mb':a.warm_mb,'prefetch_window':0}}
if a.check: config['UserCheck']=str(a.check.resolve())
(work/'config.json').write_text(json.dumps(config,indent=2))
proc = None
profiler = None
profile_log = None
commands = []
started = time.monotonic()

def limits():
    resource.setrlimit(resource.RLIMIT_AS,(3*1024**3,3*1024**3))
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))

env=os.environ|{'LC_ALL':'C','LANG':'C'}
if a.seed_shim:
    env['LD_PRELOAD']=str(a.seed_shim.resolve())+(':'+env['LD_PRELOAD'] if env.get('LD_PRELOAD') else '')
    env['DETSIM_BENCH_SEED']=str(a.seed)

try:
    with (work/'transcript.log').open('wb') as log:
        proc = subprocess.Popen([str(a.tracer.resolve()),'-n','-c',str(work/'config.json')],
            cwd=work,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
            start_new_session=True,env=env,preexec_fn=limits)
        def sample():
            try:
                status=Path(f'/proc/{proc.pid}/status').read_text()
                m=re.search(r'VmHWM:\s+(\d+)',status)
                if m: result['peak_rss_kib']=max(result['peak_rss_kib'],int(m[1]))
            except FileNotFoundError: pass
        def prompt(command=None, budget=None):
            if command is not None:
                commands.append(command);proc.stdin.write((command+'\n').encode());proc.stdin.flush()
            issued=time.monotonic()
            reply=bytearray(); deadline=min(started+a.timeout,time.monotonic()+(budget or a.timeout))
            while time.monotonic()<deadline:
                sample()
                if select.select([proc.stdout],[],[],0.05)[0]:
                    chunk=os.read(proc.stdout.fileno(),65536)
                    if not chunk: raise RuntimeError('tracer exited before prompt: '+reply.decode(errors='replace')[-1500:])
                    log.write(chunk);log.flush();reply.extend(chunk)
                    if command and command.startswith('dfs ') and 'dfs_until_report_s' not in result and b'Complete explore all' in reply:
                        result['dfs_until_report_s']=time.monotonic()-issued
                    if reply.endswith(b'(ptmc) '):
                        text=reply.decode(errors='replace')
                        if any(marker in text for marker in ('Fatal signal', 'stopped for signal',
                                                           'Reach illegal state', 'Stopped for illegal state')):
                            raise RuntimeError(text[-1500:])
                        return text
            raise TimeoutError('command timed out: '+str(command))
        prompt();result['startup_s']=time.monotonic()-started
        def cpu_seconds(pid=None, main=False):
            pid = proc.pid if pid is None else pid
            path = f'/proc/{pid}/task/{pid}/stat' if main else f'/proc/{pid}/stat'
            fields=Path(path).read_text().rsplit(')',1)[1].split()
            return (int(fields[11])+int(fields[12]))/os.sysconf('SC_CLK_TCK')
        cpu_start=cpu_seconds()
        begin=time.monotonic()
        last=['']*3
        for r in range(a.rounds):
            for node in range(3):
                prompt(f'sw {node}')
                advance = node==0 and ('bind(' in last[node] or ('recvfrom(' in last[node] and '= -11' in last[node]))
                reply=prompt('si '+('1' if advance else '0'))
                if 'has exited' in reply: raise RuntimeError(reply)
                result['steps']+=1
                last[node]=reply
        result['manual_s']=time.monotonic()-begin
        result['manual_cpu_s']=cpu_seconds()-cpu_start
        result['manual_steps_per_s']=result['steps']/result['manual_s']
        result['nodes']=[]
        for node in range(3):
            prompt(f'sw {node}')
            info={'node':node,'threads':prompt('thread')}
            for expr in ['((raft_server*)raft)->state','((raft_server*)raft)->current_term',
                         '((raft_server*)raft)->commit_idx','((raft_server*)raft)->last_applied_idx',
                         '((raft_server*)raft)->snapshot_last_idx']:
                text=prompt('p '+expr)
                match=re.search(r' = (-?\d+)\n',text)
                if not match: raise RuntimeError('cannot inspect '+expr+': '+text)
                info[expr]=int(match[1])
            result['nodes'].append(info)
        roles=[node['((raft_server*)raft)->state'] for node in result['nodes']]
        commits=[node['((raft_server*)raft)->commit_idx'] for node in result['nodes']]
        result['leader_count']=roles.count(4)
        if a.rounds>=100 and (roles.count(4)!=1 or min(commits)<1):
            raise RuntimeError('schedule did not establish leader and replication')
        blob=(work/'sstate/packed.dat').read_bytes()
        offset=0
        while offset < len(blob):
            magic,key,size,checksum=struct.unpack_from('<IQII',blob,offset)
            offset+=20
            if magic!=0x53534545 or size!=40: raise RuntimeError('unexpected NP3 metadata record')
            result['error_bound_after_manual']=struct.unpack_from('<i',blob,offset+36)[0]
            offset+=size
        if a.depth and result['error_bound_after_manual']<=0:
            raise RuntimeError('manual schedule exhausted search error budget')
        if a.depth:
            if a.perf:
                profile_log = (work/'perf.log').open('wb')
                prefix = ['sudo', '-n'] if a.perf_sudo else []
                profiler = subprocess.Popen(prefix+[str(a.perf.resolve()), 'record',
                    '-e', 'cpu-clock', '--clockid', 'mono', '-F', '199', '--call-graph', 'dwarf,32768',
                    '-p', str(proc.pid), '-o', str(work/'perf.data')],
                    stdout=profile_log, stderr=profile_log, start_new_session=True)
                # Startup is outside the timed DFS; abort on permission/tool failures.
                time.sleep(0.3)
                if profiler.poll() is not None:
                    raise RuntimeError('perf failed: '+(work/'perf.log').read_text())
            cpu_start=cpu_seconds()
            main_cpu_start=cpu_seconds(main=True)
            tracee_pids=[int(re.search(r'pid=(\d+)',node['threads'])[1]) for node in result['nodes']]
            tracee_cpu_start=sum(cpu_seconds(pid) for pid in tracee_pids)
            begin=time.monotonic();reply=prompt(f'dfs {a.depth}');end=time.monotonic();result['dfs_s']=end-begin
            result['dfs_cpu_s']=cpu_seconds()-cpu_start
            result['dfs_main_cpu_s']=cpu_seconds(main=True)-main_cpu_start
            result['dfs_tracee_cpu_s']=sum(cpu_seconds(pid) for pid in tracee_pids)-tracee_cpu_start
            if profiler:
                if a.perf_sudo:
                    subprocess.run(['sudo', '-n', 'kill', '-INT', str(profiler.pid)], check=True)
                else:
                    profiler.send_signal(signal.SIGINT)
                if profiler.wait(timeout=15) not in (0, 130, -signal.SIGINT):
                    raise RuntimeError('perf recording failed')
                profiler = None
                result['perf_data'] = str(work/'perf.data')
                result['perf_window_monotonic_s'] = [begin, end]
            result['dfs_history_s']=result['dfs_s']-result.get('dfs_until_report_s',result['dfs_s'])
            (work/'dfs-report.txt').write_text(reply)
            experiment=re.search(r'SOFT_DIRTY (\{[^\n]+\})', reply)
            if experiment: result['soft_dirty']=json.loads(experiment[1])
            result['dfs_report_tail']=reply[reply.rfind('Time elapsed:'):]
            m=re.search(r'Complete explore all (\d+) sys_states \(new: (\d+)\)',reply)
            if not m: raise RuntimeError('DFS did not report normal bounded completion: '+reply[-2000:])
            result['dfs_states']=int(m[1]);result['dfs_new_states']=int(m[2])
            result['dfs_states_per_s']=int(m[1])/result['dfs_s']
            for label,key in [('Total save time','save_ms'),('Total restore time','restore_ms')]:
                timing=re.search(label+r':\s+(\d+) ms',reply)
                if timing: result[key]=int(timing[1])
            stats=re.search(r'loads=(\d+) L1=(\d+) L2=(\d+) disk=(\d+).*?saves=(\d+) new=(\d+) dedup=([\d.]+)%',reply)
            if stats:
                result['storage_stats']=dict(zip(['loads','l1_hits','l2_hits','disk_reads','saves','new_snapshots'],map(int,stats.groups()[:6])))
                result['storage_stats']['dedup_percent']=float(stats[7])
                reference=re.search(r'reference=(\d+)',reply)
                if reference: result['storage_stats']['reference_hits']=int(reference[1])
        sample();begin=time.monotonic()
        proc.stdin.write(b'q\n');proc.stdin.flush()
        tail,_=proc.communicate(timeout=30);log.write(tail)
        result['shutdown_s']=time.monotonic()-begin;result['tracer_exit']=proc.returncode
        if proc.returncode: raise RuntimeError('tracer nonzero exit')
        result['passed']=True
except Exception as e:
    result['error']=str(e)
finally:
    try:
        if profiler and profiler.poll() is None:
            if a.perf_sudo:
                subprocess.run(['sudo', '-n', 'kill', '-INT', str(profiler.pid)], check=True)
            else:
                profiler.send_signal(signal.SIGINT)
            profiler.wait(timeout=15)
    except Exception as e:
        result['passed'] = False
        result['error'] = result.get('error', '')+'; profiler cleanup: '+str(e)
        # The profiler owns a separate session; never let its failure skip tracee cleanup.
        try:
            if a.perf_sudo:
                subprocess.run(['sudo', '-n', 'kill', '-KILL', '--', str(-profiler.pid)], check=True)
            else:
                os.killpg(profiler.pid, signal.SIGKILL)
            profiler.wait(timeout=2)
        except Exception as cleanup_error:
            result['error'] += '; profiler kill: '+str(cleanup_error)
    if profile_log: profile_log.close()
    if proc:
        try: os.killpg(proc.pid,signal.SIGKILL)
        except ProcessLookupError: pass
        proc.wait(timeout=5)
    result['elapsed_s']=time.monotonic()-started
    result['store_bytes']=sum(f.stat().st_size for d in ('memory','sstate') for f in (work/d).rglob('*') if f.is_file())
    (work/'commands.txt').write_text('\n'.join(commands)+'\n')
    (work/'result.json').write_text(json.dumps(result,indent=2))
    print(json.dumps(result,indent=2))
raise SystemExit(0 if result['passed'] else 1)
