#!/usr/bin/env python3
"""Bounded three-voter RedisLabs/raft conflicting-suffix repair case."""
import argparse
import json
from pathlib import Path
import shlex
import sys
import tempfile

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from _support import session as common
from redisraft import run as adapter
from redisraft.search import observe_logs as log_observation
from raft.run import leader_clock_ready
from redisraft.search import search_siblings

ROOT = adapter.ROOT
DIAGNOSTIC = 'redisraft.safety: property violation: conflicting committed entry 2'
MUTATION = ('if (ety->term != req->prev_log_term) {',
            'if (0 && ety->term != req->prev_log_term) {')
ALPHA = b'set alpha 17\0'
BETA = b'set beta 29\0'


def entry(index, term, kind, payload=b''):
    return dict(index=index, term=term, type=kind, data_len=len(payload), bytes_hex=payload.hex())


INITIAL = [entry(1, 1, 4), entry(2, 1, 0, ALPHA)]
REPAIRED = [entry(1, 1, 4), entry(2, 2, 4), entry(3, 2, 0, BETA)]


def observation(session):
    nodes = log_observation(session)
    for n in nodes:
        node = n['node']
        prefix = f'repair[{node}].'
        n['repair'] = {field: adapter.integer(session, node, prefix + field)
                       for field in ('phase', 'dropped', 'healed_requests', 'requests', 'replies', 'rejected')}
        # Only A receives repair traffic; B/C counters still prove saved routing phases.
        if node != 0:
            continue
        for name, fields in (
            ('last_request', ('kind', 'from', 'to', 'append.term', 'append.prev_log_idx',
                              'append.prev_log_term', 'append.leader_commit', 'append.n_entries')),
            ('last_reply', ('kind', 'from', 'to', 'appended.term', 'appended.success', 'appended.current_idx'))):
            n['repair'][name] = {field: adapter.integer(session, node, prefix + name + '.' + field)
                                for field in fields}
    return nodes


def root_valid(nodes):
    if len(nodes) != 3 or [n.get('node') for n in nodes] != [0, 1, 2]:
        return False
    for i, n in enumerate(nodes):
        if (n.get('entries') != (REPAIRED if i else INITIAL) or
            n.get('current_term') != (2 if i else 1) or n.get('state') != (1 if i == 2 else 4) or
            n.get('commit_idx') != (3 if i else 1) or n.get('last_applied_idx') != (3 if i else 1) or
            n.get('snapshot_last_idx') != 0 or n.get('repair', {}).get('healed_requests') != 0 or
            n.get('repair', {}).get('phase') != (2 if i else 1)):
            return False
    return True


def terminal(reply, nodes, variant):
    common.require(not any(marker in reply for marker in
        ('DetSim state/execution error:', 'Fatal signal', ': unavailable:', ': error:')), 'invalid repair outcome: ' + reply)
    common.require(len(nodes) == 3 and [n.get('node') for n in nodes] == [0, 1, 2], 'missing repair nodes')
    illegal = any(marker in reply for marker in ('Reach illegal state', 'Stopped for illegal state'))
    violations = [line for line in reply.splitlines() if ': property violation:' in line]
    a, b, c = nodes
    common.require(b['entries'] == REPAIRED and c['entries'] == REPAIRED and
        all(n['commit_idx'] == 3 and n['last_applied_idx'] == 3 and n['current_term'] == 2 for n in (b, c)),
        'repair changed committed B/C reference logs')
    receipt = a['repair']
    def successful_repair_reply():
        return (a['current_term'] == 2 and a['state'] == 1 and
            all(receipt['last_reply'].get(field) == expected for field, expected in
                (('kind', 4), ('from', 0), ('to', 1), ('appended.term', 2),
                 ('appended.success', 1), ('appended.current_idx', 3))) and
            all(receipt['last_request'].get(field) == expected for field, expected in
                (('kind', 3), ('from', 1), ('to', 0), ('append.term', 2))))
    if variant == 'mutant' and illegal:
        common.require(violations == [DIAGNOSTIC] and successful_repair_reply() and
            a['entries'] == INITIAL + [entry(3, 2, 0, BETA)] and
            a['commit_idx'] == 3 and 1 <= a['last_applied_idx'] <= 3 and
            a['repair']['healed_requests'] >= 2 and a['repair']['last_request']['append.prev_log_idx'] == 2 and
            a['repair']['last_request']['append.prev_log_term'] == 2 and
            a['repair']['last_request']['append.leader_commit'] == 3 and
            a['repair']['last_request']['append.n_entries'] == 1,
            'wrong committed suffix conflict or diagnostic: ' + reply)
        return True
    common.require(not illegal and not violations, 'unexpected repair safety violation: ' + reply)
    complete = all(n['entries'] == REPAIRED and n['commit_idx'] == 3 and n['last_applied_idx'] == 3 for n in nodes)
    if complete:
        common.require(variant == 'healthy' and successful_repair_reply() and
            a['repair']['healed_requests'] >= 3 and a['repair']['rejected'] >= 2 and
            a['repair']['last_request']['append.prev_log_idx'] == 1 and
            a['repair']['last_request']['append.prev_log_term'] == 1 and
            a['repair']['last_request']['append.n_entries'] == 2, 'repair completed without genuine mismatch rejection')
        return True
    common.require(not (variant == 'mutant' and a['commit_idx'] >= 2), 'mutant conflict missed checker')
    return False


def model_case(tracer, binary, checker, choose, directory, backend, variant, timeout, depth):
    row = dict(target='redisraft-log-repair', variant=variant, backend=backend, NP=3,
               artifacts=str(directory), depth=depth,
               scope='fixed three voters; real term2 election and conflicting suffix repair; no crash or compaction')
    session = common.Session(tracer, [str(binary)], [checker, choose], directory, backend, timeout,
        tracees=[[str(binary), str(i)] for i in range(3)],
        addresses=['192.168.0.1', '192.168.0.2', '192.168.0.3'],
        choose_points=[{'syscall': 96, 'choose': 2}])
    events, last = [], [''] * 3
    def value(node, expr):
        return adapter.integer(session, node, expr)
    def step(node, choice=0):
        session.send('sw ' + str(node))
        before = session.current_hash()
        reply = session.send('si ' + str(choice))
        common.require(not any(s in reply for s in ('DetSim state/execution error:', 'Fatal signal', 'illegal state', ': property violation:')), 'invalid repair prefix: ' + reply)
        last[node] = reply
        events.append(dict(node=node, choice=choice, predecessor=before, successor=session.current_hash(), reply=reply))
        (directory/'prefix-events.json').write_text(json.dumps(events, indent=2)+'\n')
        return reply
    def until(node, predicate, maximum=90):
        for _ in range(maximum):
            if predicate():
                return
            step(node)
        raise common.CaseFailure(f'repair prefix checkpoint failed for node {node}: {last[node]}')
    def timeout_node(node):
        for _ in range(90):
            if leader_clock_ready(last[node]):
                step(node, 1)
                return
            step(node)
        raise common.CaseFailure('missing timeout clock checkpoint')
    try:
        session.send()
        for n in range(3):
            until(n, lambda n=n: value(n, '(void*)r') != 0)
            until(n, lambda n=n: leader_clock_ready(last[n]))
        timeout_node(0)
        until(0, lambda: value(0, 'sent_events') >= 2)
        until(1, lambda: value(1, 'received_events') >= 1 and value(1, 'sent_events') >= 1)
        until(0, lambda: value(0, 'sent_events') >= 4)
        until(2, lambda: value(2, 'received_events') >= 2 and value(2, 'sent_events') >= 2)
        until(1, lambda: value(1, 'received_events') >= 2 and value(1, 'sent_events') >= 2)
        until(0, lambda: value(0, 'sent_events') >= 6)
        for n in (1, 2):
            until(n, lambda n=n: value(n, f'repair[{n}].phase') == 1)
        until(0, lambda: value(0, 'repair[0].phase') == 1)
        # C first releases its old leader lease. B handles C's pre-vote before
        # its own timeout, so C cannot win using B's later pre-vote response.
        timeout_node(2)
        until(2, lambda: value(2, '((raft_server*)r)->state') == 2)
        b_receipts = value(1, 'received_events')
        until(1, lambda: value(1, 'received_events') > b_receipts and value(1, 'last_received.kind') == 1)
        until(1, lambda: value(1, 'last_sent.kind') == 2)
        timeout_node(1)
        until(1, lambda: value(1, '((raft_server*)r)->state') == 2)
        for _ in range(180):
            if value(1, '((raft_server*)r)->state') == 4 and value(1, '((raft_server*)r)->current_term') == 2:
                break
            step(2); step(1)
        else:
            raise common.CaseFailure('real B/C term2 election did not complete')
        for _ in range(240):
            if all(value(n, '((raft_server*)r)->last_applied_idx') == 3 for n in (1, 2)):
                break
            step(1, int(leader_clock_ready(last[1]))); step(2)
        else:
            raise common.CaseFailure('B/C did not commit and apply term2 suffix')
        root = session.current_hash()
        initial = observation(session)
        common.require(root_valid(initial), 'invalid preheal root: ' + repr(initial))
        row.update(root=root, root_observation=initial, root_before_repair=True,
            search_initial_budget=adapter.saved_budget(directory, root),
            binary_sha256=common.digest(binary), checker_sha256=common.digest(checker),
            choose_sha256=common.digest(choose), tracer_sha256=common.digest(tracer),
            search_driver_sha256=common.digest(Path(__file__).with_name('search.py')),
            bounds=dict(depth=depth, wall_seconds=timeout, address_space_bytes=2*1024**3))
        common.require(row['search_initial_budget'] > 0, 'repair prefix exhausted timeout budget')
        (directory/'root-observation.json').write_text(json.dumps(initial, indent=2)+'\n')
        search_siblings(session, row, root, variant, depth, observe=observation, outcome=terminal,
                        clock_actor=1, success_label='bounded_log_repair')
    except Exception as error:
        row.update(common.classify_error(error, session.log))
    finally:
        row['steps'] = len(events)
        row['tracer_exit'] = session.close(timeout=30)
        row['owned_pids_remaining'] = [pid for pid in row.get('owned_pids_before_close', []) if Path(f'/proc/{pid}').exists()]
        if row['tracer_exit'] != 0 and row.get('status') in ('pass', 'violation'):
            row.update(status='failed', diagnostic='tracer cleanup failed')
    return row


def accepted_rows(rows, variants, backends, native_only=False):
    from redisraft.search import accepted_search_row
    expected = {(v, None if native_only else b) for v in variants
                for b in ((None,) if native_only else backends)}
    actual = [(r.get('variant'), None if native_only else r.get('backend')) for r in rows]
    if len(actual) != len(expected) or set(actual) != expected:
        return False
    for row in rows:
        if row.get('native') != 'pass':
            return False
        if native_only:
            if row.get('status') != 'pass':
                return False
            continue
        if not root_valid(row.get('root_observation', [])) or row.get('root_before_repair') is not True:
            return False
        if not accepted_search_row(row, outcome=terminal):
            return False
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'build/redisraft-repair-cases')
    parser.add_argument('--tracer', type=Path)
    parser.add_argument('--native-only', action='store_true')
    parser.add_argument('--backend', choices=('full', 'pages', 'all'), default='all')
    parser.add_argument('--variant', choices=('healthy', 'mutant', 'all'), default='all')
    parser.add_argument('--timeout', type=float, default=600)
    parser.add_argument('--depth', type=int, default=96)
    parser.add_argument('--cc', default='/usr/bin/gcc')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='run-', dir=args.output.resolve()))
    variants = ('healthy', 'mutant') if args.variant == 'all' else (args.variant,)
    backends = ('full', 'pages') if args.backend == 'all' else (args.backend,)
    rows = []
    try:
        if not args.native_only:
            common.require(args.tracer and args.tracer.exists(), 'matching NP3 --tracer required')
            checker, choose = work/'check.so', work/'choose.so'
            common.build_plugin(ROOT/'examples/redisraft/plugins/check.cpp', checker, ['/usr/bin/g++'])
            common.build_plugin(ROOT/'examples/redisraft/plugins/choose.cpp', choose, ['/usr/bin/g++'])
        for variant in variants:
            owned, manifest = adapter.build_variant(work, variant, shlex.split(args.cc),
                fixture=adapter.HERE/'redisraft_repair_fixture.c', mutation=MUTATION)
            manifest['scenario_runner_sha256'] = common.digest(Path(__file__))
            manifest['profile'].update(fixed_voters=3, udp_queue_packets=3, crashes=False,
                search='manual six actor/preset siblings, beam1; builtin DFS budget cutoff not applied')
            (owned/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
            if args.native_only:
                rows.append(dict(variant=variant, status='pass', native='pass', manifest=str(owned/'manifest.json')))
            else:
                for backend in backends:
                    row = model_case(args.tracer.resolve(), owned/'raft-case', checker, choose,
                        work/(variant+'-'+backend), backend, variant, args.timeout, args.depth)
                    row.update(native='pass', manifest=str(owned/'manifest.json'),
                               scenario_runner_sha256=manifest['scenario_runner_sha256'])
                    rows.append(row)
        result = dict(passed=accepted_rows(rows, variants, backends, args.native_only), workdir=str(work), rows=rows)
    except Exception as error:
        result = dict(passed=False, workdir=str(work), rows=rows, diagnostic=str(error))
    (work/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
