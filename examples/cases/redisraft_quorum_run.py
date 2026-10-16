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

ROOT = adapter.ROOT
DIAGNOSTIC = 'redisraft.replication-quorum: property violation: committed NORMAL entry 2 lacks replica quorum'
COMMAND = b'set alpha 17\0'


def observation(session):
    nodes = adapter.inspect(session)
    for n in nodes:
        node = n['node']
        log = adapter.integer(session, node, '(long)((raft_server*)r)->log')
        f = {name: adapter.integer(session, node, f'(long)((raft_log_t*){log})->{name}')
             for name in ('base', 'count', 'size', 'front', 'entries')}
        common.require(f['base'] == 0 and 0 <= f['count'] <= 3 and f['size'] > 0,
                       'quorum profile requires bounded uncompacted logs')
        n['entries'] = []
        for offset in range(f['count']):
            slot = (f['front'] + offset) % f['size']
            ptr = adapter.integer(session, node, f'(long)((raft_entry_t**){f["entries"]})[{slot}]')
            entry = {name: adapter.integer(session, node, f'((raft_entry_t*){ptr})->{name}')
                     for name in ('term', 'type', 'data_len')}
            common.require(0 <= entry['data_len'] <= 32, 'unbounded command payload')
            entry['bytes_hex'] = bytes(adapter.integer(session, node,
                f'(long)((raft_entry_t*){ptr})->data[{i}]') & 255
                for i in range(entry['data_len'])).hex()
            entry['index'] = offset + 1
            n['entries'].append(entry)
    return nodes


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
        source = root
        attempts = []
        retained = []
        terminal_edge = None
        row['budget_policy'] = 'manual si siblings; no builtin DFS budget cutoff; timeout plugin still enforces its budget'
        row['search_initial_budget'] = adapter.saved_budget(session.directory, root)
        aliases = 0
        counters = dict(attempted_actions=0, completed_layers=0, depth_cutoff=True, partial_layer=True, beam_width=1)
        row['search_counters'] = counters
        leader_clock = False
        for layer in range(depth):
            candidates = []
            source_seen = set()
            for node in range(3):
                for preset in range(2):
                    common.require('Loaded state' in session.send('load ' + source), 'missing search source')
                    common.require(session.current_hash() == source, 'loaded search source identity differs')
                    switched = session.send('sw ' + str(node))
                    common.require('Switched to process ' + str(node) + ' ' in switched, 'actor switch failed')
                    reply = session.send('si ' + str(preset))
                    counters.update(attempted_actions=counters['attempted_actions']+1, partial_layer=True)
                    row['last_attempt'] = dict(layer=layer,predecessor=source,node=node,preset=preset,reply=reply)
                    (session.directory/'last-attempt.json').write_text(json.dumps(row['last_attempt'],indent=2)+'\n')
                    common.require('DetSim state/execution error:' not in reply and 'Fatal signal' not in reply,
                                   'sibling execution error: ' + reply)
                    successor = session.current_hash()
                    common.require(successor is not None, 'missing successor identity')
                    history = session.send('info current')
                    edges = re.findall(r'([0-9a-f]{16}) => ([0-9a-f]{16})', history)
                    common.require(edges and edges[-1][1] == successor, 'missing canonical successor history')
                    canonical_matches = edges[-1][0] == source
                    actor = re.search(r'Tracee (\d+)(?: \(choose (\d+)\))?:', history)
                    common.require(actor is not None, 'missing canonical actor metadata')
                    # State history retains the FIRST incoming edge for a deduplicated
                    # target. The verified load/sw/si reply is our local edge ledger.
                    nodes = observation(session)
                    done = terminal(reply, nodes, variant)
                    edge = dict(layer=layer, predecessor=source, successor=successor,
                                node=node, preset=preset, source_verified=True, actor_switch_verified=True,
                                canonical_edge=list(edges[-1]), canonical_actor=int(actor[1]),
                                canonical_choice=None if actor[2] is None else int(actor[2]),
                                canonical_source_matches=canonical_matches, terminal=done, observation=nodes, reply=reply,
                                verdict="violation" if ": property violation:" in reply else "pass",
                                diagnostic=next((line for line in reply.splitlines() if ': property violation:' in line), None),
                                clock_ready=bool(leader_clock_ready(reply)))
                    attempts.append(edge)
                    counters.update(attempted_actions=len(attempts), partial_layer=len(attempts)%6 != 0,
                                    completed_layers=len(attempts)//6)
                    (session.directory/'search-edges.json').write_text(json.dumps(attempts,indent=2)+'\n')
                    if successor in source_seen:
                        aliases += 1
                    source_seen.add(successor)
                    if done:
                        if terminal_edge is None:
                            terminal_edge = edge
                    else:
                        # Keep a fixed round-robin actor, ordinary clocks. Every sibling
                        # above is still executed and checked, including pruned timeouts.
                        candidates.append(edge)
            (session.directory / 'search-edges.json').write_text(json.dumps(attempts, indent=2)+'\n')
            (session.directory / 'retained-sources.json').write_text(json.dumps(retained, indent=2)+'\n')
            if terminal_edge:
                break
            preferred = (layer % 3, int(layer % 3 == 0 and leader_clock))
            selected = next((e for e in candidates if (e['node'], e['preset']) == preferred), None)
            common.require(selected is not None, 'beam exhausted without terminal edge')
            retained.append(selected)
            if selected['node'] == 0:
                leader_clock = selected['clock_ready']
            source = selected['successor']
        row['search_edges'] = attempts
        row['retained_sources'] = retained
        counters['depth_cutoff'] = terminal_edge is None
        common.require(terminal_edge is not None, 'depth cutoff before NORMAL terminal edge')
        verdict = terminal_edge['verdict']
        replays = []
        for _ in range(3):
            common.require('Loaded state' in session.send('load ' + terminal_edge['predecessor']),
                           'missing terminal predecessor')
            common.require(session.current_hash() == terminal_edge['predecessor'], 'loaded replay source identity differs')
            session.send('sw ' + str(terminal_edge['node']))
            reply = session.send('si ' + str(terminal_edge['preset']))
            common.require(session.current_hash() == terminal_edge['successor'] and
                observation(session) == terminal_edge['observation'] and
                terminal(reply, terminal_edge['observation'], variant),
                'terminal replay changed hash/verdict/observation')
            replays.append(dict(predecessor=terminal_edge['predecessor'], node=terminal_edge['node'],
                preset=terminal_edge['preset'], diagnostic=next((line for line in reply.splitlines() if ': property violation:' in line), None),
                successor=session.current_hash(),
                verdict='violation' if ': property violation:' in reply else 'pass',
                observation=observation(session)))
            common.require(replays[-1]['verdict'] == verdict, 'replay verdict differs')
        (session.directory / 'search-edges.json').write_text(json.dumps(attempts, indent=2) + '\n')
        children = Path(f'/proc/{session.proc.pid}/task/{session.proc.pid}/children').read_text().split()
        row['owned_pids_before_close'] = [session.proc.pid] + [int(pid) for pid in children]
        row.update(status=verdict, search='bounded_counterexample' if variant == 'mutant' else 'bounded_first_NORMAL_commit',
                   terminal_edge=terminal_edge, replay_records=replays, replay='same_process_hash_verdict_observation_match_3x',
                   search_counters=dict(attempted_actions=len(attempts), source_layers=len(retained)+1,
                       unique_successors=sum(len({e['successor'] for e in attempts if e['layer']==i})
                                             for i in range(len(retained)+1)),
                       alias_actions=aliases, retained_sources=len(retained),
                       pruned_distinct_children=sum(len({e['successor'] for e in attempts if e['layer']==i}) for i in range(len(retained)+1))-len(retained)-1,
                       pruned_actions=len(attempts)-len(retained)-1,
                       depth_cutoff=False, partial_layer=False, beam_width=1),
                   tracer_sha256=common.digest(session.proc.args[0]),
                   config_sha256=common.digest(session.directory / 'config.json'))
    return run


def accepted_rows(rows, variants, backends, native_only=False):
    expected = {(v, None if native_only else b) for v in variants
                for b in ((None,) if native_only else backends)}
    actual = [(r.get('variant'), None if native_only else r.get('backend')) for r in rows]
    if len(actual) != len(expected) or set(actual) != expected:
        return False
    for row in rows:
        if native_only:
            continue
        if row.get('root_leader_matches') != [1,0,0]:
            return False
        for name in ('scenario_runner_sha256','binary_sha256','checker_sha256','choose_sha256',
                     'config_sha256','tracer_sha256'):
            if not re.fullmatch('[0-9a-f]{64}', row.get(name, '')):
                return False
        edges, retained = row.get('search_edges', []), row.get('retained_sources', [])
        counters = row.get('search_counters', {})
        pids = row.get('owned_pids_before_close', [])
        if len(pids) != 4 or any(type(pid) is not int or pid <= 0 for pid in pids) or len(set(pids)) != 4 or row.get('owned_pids_remaining') != []:
            return False
        initial = row.get('root_observation', [])
        if (len(initial) != 3 or {n.get('node') for n in initial} != {0,1,2} or
            any(n.get('commit_idx') != 0 or n.get('entries') !=
                [dict(index=1,term=1,type=4,data_len=0,bytes_hex='')] for n in initial)):
            return False
        if (not row.get('root_before_NORMAL') or not edges or len(edges)%6 or
            counters.get('attempted_actions') != len(edges) or counters.get('partial_layer') is not False or
            counters.get('depth_cutoff') is not False or row.get('terminal_edge') not in edges):
            return False
        layers = len(edges)//6
        distinct = sum(len({edge.get('successor') for edge in edges[i*6:(i+1)*6]}) for i in range(layers))
        expected_counters = dict(source_layers=layers, unique_successors=distinct,
            alias_actions=len(edges)-distinct, retained_sources=len(retained),
            pruned_actions=len(edges)-len(retained)-1,
            pruned_distinct_children=distinct-len(retained)-1, beam_width=1)
        if any(counters.get(name) != value for name,value in expected_counters.items()):
            return False
        source = row.get('root')
        for layer in range(len(edges)//6):
            siblings = edges[layer*6:(layer+1)*6]
            if ({(e.get('node'),e.get('preset')) for e in siblings} !=
                {(node,preset) for node in range(3) for preset in range(2)} or
                any(e.get('predecessor') != source or e.get('layer') != layer for e in siblings)):
                return False
            if layer < len(retained):
                if retained[layer] not in siblings:
                    return False
                source = retained[layer]['successor']
        if (row['terminal_edge'].get('terminal') is not True or
            row['terminal_edge'].get('verdict') != row.get('status') or
            any(e.get('terminal') for e in edges[:-6])):
            return False
        if row['terminal_edge'].get('layer') != len(edges)//6-1 or row['terminal_edge'].get('predecessor') != source:
            return False
        for edge in edges:
            if (any(marker in edge.get('reply','') for marker in ('DetSim state/execution error:', 'Fatal signal')) or
                edge.get('source_verified') is not True or edge.get('actor_switch_verified') is not True or
                not re.search(r'(?:^|\n)(?:gettimeofday|sendto|recvfrom)\([^\n]*\) = -?\d+', edge.get('reply',''))):
                return False
            try:
                if terminal(edge.get('reply',''), edge['observation'], row['variant']) != edge.get('terminal'):
                    return False
            except (common.CaseFailure, KeyError):
                return False
            if edge.get('verdict') != ('violation' if ': property violation:' in edge.get('reply','') else 'pass'):
                return False
        if len(retained) != len(edges)//6-1:
            return False
    return all(r.get('status') == ('violation' if r.get('variant') == 'mutant' and not native_only else 'pass')
        and r.get('native') == 'pass' and (native_only or
        (r.get('tracer_exit') == 0 and r.get('replay') == 'same_process_hash_verdict_observation_match_3x'
         and len(r.get('replay_records', [])) == 3
         and all(e.get('predecessor') == r['terminal_edge']['predecessor'] and
                 e.get('node') == r['terminal_edge']['node'] and e.get('preset') == r['terminal_edge']['preset'] and
                 e.get('diagnostic') == r['terminal_edge'].get('diagnostic') and
                 e.get('verdict') == r['status'] and
                 e.get('successor') == r['terminal_edge']['successor'] and
                 e.get('observation') == r['terminal_edge']['observation']
                 for e in r['replay_records']))) for r in rows)


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
                        choose_sha256=common.digest(choose))
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
