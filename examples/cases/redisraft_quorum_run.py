#!/usr/bin/env python3
"""Bounded real RedisLabs/raft NORMAL replication search (fixed three voters)."""
import argparse
import json
import re
from pathlib import Path
import shlex
import sys
import tempfile

import run as common
import redisraft_run as adapter
from raft_run import leader_clock_ready
from raft_search import observe_logs as observation, search_siblings, accepted_search_row

ROOT = adapter.ROOT
DIAGNOSTIC = 'redisraft.replication-quorum: property violation: committed NORMAL entry 2 lacks replica quorum'
COMMAND = b'set alpha 17\0'





def terminal(reply, nodes, variant):
    common.require(not any(marker in reply for marker in ('DetSim state/execution error:', 'Fatal signal', ': unavailable:', ': error:')), 'invalid sibling outcome: ' + reply)
    illegal = any(marker in reply for marker in ('Reach illegal state', 'Stopped for illegal state'))
    violations = [line for line in reply.splitlines() if ': property violation:' in line]
    entry = next((e for e in nodes[0]['entries'] if e['index'] == 2), None)
    if not entry:
        common.require(not illegal and not violations, 'unexpected violation: ' + reply)
        return False
    common.require(len(nodes) == 3 and {n['node'] for n in nodes} == {0,1,2}, 'missing node observation')
    common.require(entry['term'] == 1 and entry['type'] == 0 and
                   entry['bytes_hex'] == COMMAND.hex(), 'unexpected first NORMAL entry')
    copies = sum(entry in node['entries'] for node in nodes)
    committed = nodes[0]['commit_idx'] >= 2
    if variant == 'mutant' and illegal:
        common.require(violations == [DIAGNOSTIC] and committed and copies == 1,
                       'wrong quorum violation or evidence: ' + reply)
        return True
    common.require(not illegal and not violations, 'unexpected safety violation: ' + reply)
    if variant == 'healthy' and committed and nodes[0]['last_applied_idx'] >= 2:
        common.require(copies >= 2, 'healthy NORMAL commit lacks exact replica bytes')
        return True
    common.require(not (variant == 'mutant' and committed), 'mutant commit missed public ABI violation')
    return False


def drain_election_replies(step):
    # Drain before A can emit its append: modeled UDP capacity is three packets.
    for _ in range(30):
        if leader_clock_ready(step(1)):
            return
    raise common.CaseFailure('B election queue did not drain')


def search_hook(variant, depth):
    def run(session, row, step, until, value, received, events):
        # A is stopped in its initial callback stream, before another periodic/generate.
        until(0, lambda: value(0, 'sent_events') >= 8)
        for node in (1, 2):
            until(node, lambda node=node: value(node, 'last_received.kind') == 3 and
                  value(node, 'last_sent.kind') == 4)
        root = session.current_hash()
        initial = observation(session)
        (session.directory/'root-observation.json').write_text(json.dumps(initial,indent=2)+'\n')
        common.require(all(n['commit_idx'] == 0 and len(n['entries']) == 1 and
            n['entries'][0]['type'] == 4 and n['entries'][0]['term'] == 1 and
            n['entries'][0]['bytes_hex'] == '' for n in initial), 'root must precede every NORMAL append')
        leader_matches=[value(0,f'((raft_server*)r)->nodes[{i}]->match_idx') for i in range(3)]
        common.require(leader_matches == [1,0,0], 'root ACKs already consumed')
        row.update(root_leader_matches=leader_matches, root_before_NORMAL=True, target='redisraft-replication-quorum', root=root, root_observation=initial,
                   scope='beam1 sibling actor/preset search before first NORMAL commit', depth=depth)
        search_siblings(session, row, root, variant, depth, observe=observation, outcome=terminal)
    return run


def accepted_rows(rows, variants, backends, native_only=False):
    expected = {(v, None if native_only else b) for v in variants
                for b in ((None,) if native_only else backends)}
    actual = [(r.get('variant'), None if native_only else r.get('backend')) for r in rows]
    if len(actual) != len(expected) or set(actual) != expected:
        return False
    for row in rows:
        if native_only:
            if row.get('status') != 'pass' or row.get('native') != 'pass':
                return False
            continue
        initial = row.get('root_observation', [])
        if (row.get('root_leader_matches') != [1,0,0] or
            len(initial) != 3 or {n.get('node') for n in initial} != {0,1,2} or
            any(n.get('commit_idx') != 0 or n.get('entries') !=
                [dict(index=1,term=1,type=4,data_len=0,bytes_hex='')] for n in initial) or
            not row.get('root_before_NORMAL') or not accepted_search_row(row, terminal)):
            return False
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/redisraft-quorum-cases')
    parser.add_argument('--tracer', type=Path)
    parser.add_argument('--native-only', action='store_true')
    parser.add_argument('--backend', choices=('full','pages','all'), default='all')
    parser.add_argument('--variant', choices=('healthy','mutant','all'), default='all')
    parser.add_argument('--timeout', type=float, default=600)
    parser.add_argument('--depth', type=int, default=72)
    parser.add_argument('--cc', default='/usr/bin/gcc')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='run-', dir=args.output.resolve()))
    rows = []
    variants = ('healthy','mutant') if args.variant == 'all' else (args.variant,)
    backends = ('full','pages') if args.backend == 'all' else (args.backend,)
    try:
        if not args.native_only:
            common.require(args.tracer and args.tracer.exists(), 'matching NP3 --tracer required')
            checker, choose = work/'quorum.so', work/'choose.so'
            common.build_plugin(ROOT/'examples/redisraft/plugins/quorum.cpp', checker, ['/usr/bin/g++'])
            common.build_plugin(ROOT/'examples/redisraft/plugins/choose.cpp', choose, ['/usr/bin/g++'])
        for variant in variants:
            owned, manifest = adapter.build_variant(work, variant, shlex.split(args.cc), quorum=True)
            manifest['scenario_runner_sha256'] = common.digest(Path(__file__))
            manifest['profile'].update(udp_queue_packets=3, terminal_command='set alpha 17', fixed_voters=3,
                search='manual six actor/preset siblings, beam1; builtin DFS budget cutoff not applied')
            (owned/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
            if args.native_only:
                rows.append(dict(variant=variant, status='pass', native='pass', manifest=str(owned/'manifest.json')))
            else:
                for backend in backends:
                    row = adapter.model_case(args.tracer.resolve(), owned/'raft-case', checker, choose,
                        work/(variant+'-'+backend), backend, variant, args.timeout, args.depth,
                        prefix_hook=search_hook(variant,args.depth), prepare_leader=drain_election_replies)
                    row['owned_pids_remaining'] = [pid for pid in row.get('owned_pids_before_close', []) if Path(f'/proc/{pid}').exists()]
                    row.update(scenario_runner_sha256=manifest['scenario_runner_sha256'], native='pass', manifest=str(owned/'manifest.json'),
                        binary_sha256=manifest['binary_sha256'], checker_sha256=common.digest(checker),
                        choose_sha256=common.digest(choose),
                        search_driver_sha256=common.digest(Path(__file__).with_name('raft_search.py')))
                    rows.append(row)
        passed = accepted_rows(rows, variants, backends, args.native_only)
        result = dict(passed=passed, workdir=str(work), rows=rows)
    except Exception as error:
        result = dict(passed=False, workdir=str(work), rows=rows, diagnostic=str(error))
    (work/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
