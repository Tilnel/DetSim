"""Shared three-process Raft case ledger, pruned sibling search and replay.

Workload helper only: manual actor/preset coverage, not builtin DFS semantics.
"""
import re
import json
from pathlib import Path
import run as common
import redisraft_run as adapter
from raft_run import leader_clock_ready

def observe_logs(session):
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

def search_siblings(session, row, root, variant, depth, *, observe, outcome, clock_actor=0,
                    success_label="bounded_first_NORMAL_commit"):
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
                nodes = observe(session)
                done = outcome(reply, nodes, variant)
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
        preferred = (layer % 3, int(layer % 3 == clock_actor and leader_clock))
        selected = next((e for e in candidates if (e['node'], e['preset']) == preferred), None)
        common.require(selected is not None, 'beam exhausted without terminal edge')
        retained.append(selected)
        if selected['node'] == clock_actor:
            leader_clock = selected['clock_ready']
        source = selected['successor']
    row['search_edges'] = attempts
    row['retained_sources'] = retained
    counters['depth_cutoff'] = terminal_edge is None
    common.require(terminal_edge is not None, 'depth cutoff before case terminal edge')
    verdict = terminal_edge['verdict']
    replays = []
    for _ in range(3):
        common.require('Loaded state' in session.send('load ' + terminal_edge['predecessor']),
                       'missing terminal predecessor')
        common.require(session.current_hash() == terminal_edge['predecessor'], 'loaded replay source identity differs')
        session.send('sw ' + str(terminal_edge['node']))
        reply = session.send('si ' + str(terminal_edge['preset']))
        common.require(session.current_hash() == terminal_edge['successor'] and
            observe(session) == terminal_edge['observation'] and
            outcome(reply, terminal_edge['observation'], variant),
            'terminal replay changed hash/verdict/observation')
        replays.append(dict(predecessor=terminal_edge['predecessor'], node=terminal_edge['node'],
            preset=terminal_edge['preset'], diagnostic=next((line for line in reply.splitlines() if ': property violation:' in line), None),
            successor=session.current_hash(),
            verdict='violation' if ': property violation:' in reply else 'pass',
            observation=observe(session)))
        common.require(replays[-1]['verdict'] == verdict, 'replay verdict differs')
    (session.directory / 'search-edges.json').write_text(json.dumps(attempts, indent=2) + '\n')
    children = Path(f'/proc/{session.proc.pid}/task/{session.proc.pid}/children').read_text().split()
    row['owned_pids_before_close'] = [session.proc.pid] + [int(pid) for pid in children]
    row.update(status=verdict, search='bounded_counterexample' if variant == 'mutant' else success_label,
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


def accepted_search_row(row, outcome):
    for name in ('scenario_runner_sha256','binary_sha256','checker_sha256','choose_sha256',
                 'config_sha256','tracer_sha256','search_driver_sha256'):
        if not re.fullmatch('[0-9a-f]{64}', row.get(name, '')):
            return False
    edges, retained = row.get('search_edges', []), row.get('retained_sources', [])
    counters = row.get('search_counters', {})
    pids = row.get('owned_pids_before_close', [])
    if len(pids) != 4 or any(type(pid) is not int or pid <= 0 for pid in pids) or len(set(pids)) != 4 or row.get('owned_pids_remaining') != []:
        return False
    if (not edges or len(edges)%6 or
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
            if outcome(edge.get('reply',''), edge['observation'], row['variant']) != edge.get('terminal'):
                return False
        except (common.CaseFailure, KeyError):
            return False
        if edge.get('verdict') != ('violation' if ': property violation:' in edge.get('reply','') else 'pass'):
            return False
    if len(retained) != len(edges)//6-1:
        return False
    return (row.get('status') == ('violation' if row.get('variant') == 'mutant' else 'pass')
        and row.get('native') == 'pass' and row.get('tracer_exit') == 0
        and row.get('replay') == 'same_process_hash_verdict_observation_match_3x'
        and len(row.get('replay_records', [])) == 3
        and all(e.get('predecessor') == row['terminal_edge']['predecessor']
            and e.get('node') == row['terminal_edge']['node'] and e.get('preset') == row['terminal_edge']['preset']
            and e.get('diagnostic') == row['terminal_edge'].get('diagnostic')
            and e.get('verdict') == row['status'] and e.get('successor') == row['terminal_edge']['successor']
            and e.get('observation') == row['terminal_edge']['observation'] for e in row['replay_records']))
