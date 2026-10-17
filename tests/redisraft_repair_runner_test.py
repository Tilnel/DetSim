"""Strict root, committed-byte conflict, sibling ledger and replay acceptance."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'examples/cases'))
import redisraft_repair_run as repair
import run as common


def nodes(root=False, mutant=False):
    result = []
    for i in range(3):
        entries = repair.INITIAL if root and i == 0 else repair.REPAIRED
        if mutant and i == 0:
            entries = repair.INITIAL + [repair.entry(3, 2, 0, repair.BETA)]
        result.append(dict(node=i, entries=copy.deepcopy(entries), state=4 if i == 1 or (root and i == 0) else 1,
            current_term=1 if root and i == 0 else 2, commit_idx=1 if root and i == 0 else 3,
            last_applied_idx=1 if root and i == 0 else 3, snapshot_last_idx=0,
            repair=dict(phase=1 if root and i == 0 else 2, dropped=1,
                healed_requests=0 if root or i else 2 if mutant else 3,
                requests=0 if root or i else 2 if mutant else 3,
                replies=0 if root or i else 2 if mutant else 3,
                rejected=0 if root or i else 1 if mutant else 2,
                last_reply={'kind': 4, 'from': 0, 'to': 1, 'appended.term': 2,
                    'appended.success': 1, 'appended.current_idx': 3},
                last_request={'kind': 3, 'from': 1, 'to': 0, 'append.term': 2,
                    'append.n_entries': 1 if mutant else 2, 'append.prev_log_idx': 2 if mutant else 1,
                    'append.prev_log_term': 2 if mutant else 1, 'append.leader_commit': 3})))
    return result


def matrix():
    result = []
    for variant in ('healthy', 'mutant'):
        for backend in ('full', 'pages'):
            mutant = variant == 'mutant'
            observed = nodes(mutant=mutant)
            reply = 'sendto(3, 1, 1, 0, 1, 16) = 352\n'
            if mutant:
                reply += repair.DIAGNOSTIC + '\nReach illegal state.\n'
            verdict = 'violation' if mutant else 'pass'
            edge = dict(source_verified=True, actor_switch_verified=True, terminal=True,
                reply=reply, verdict=verdict, diagnostic=repair.DIAGNOSTIC if mutant else None,
                predecessor='root', successor='abc', observation=observed, layer=0, node=0, preset=0)
            row = dict(variant=variant, backend=backend, status=verdict, native='pass', tracer_exit=0,
                owned_pids_before_close=[123, 124, 125, 126], owned_pids_remaining=[],
                root='root', root_before_repair=True, root_observation=nodes(root=True),
                search_edges=[edge | dict(node=n, preset=p) for n in range(3) for p in range(2)],
                retained_sources=[], terminal_edge=edge,
                search_counters=dict(attempted_actions=6, partial_layer=False, depth_cutoff=False,
                    source_layers=1, unique_successors=1, alias_actions=5, retained_sources=0,
                    pruned_actions=5, pruned_distinct_children=0, beam_width=1),
                replay='same_process_hash_verdict_observation_match_3x',
                replay_records=[dict(predecessor='root', node=0, preset=0,
                    diagnostic=edge['diagnostic'], successor='abc', observation=observed,
                    verdict=verdict) for _ in range(3)])
            for key in ('scenario_runner', 'binary', 'checker', 'choose', 'config', 'tracer', 'search_driver'):
                row[key+'_sha256'] = 'a'*64
            result.append(row)
    return result


class RepairAcceptance(unittest.TestCase):
    def test_root_requires_real_divergent_terms_bytes_and_commits(self):
        self.assertTrue(repair.root_valid(nodes(root=True)))
        for change in (
            lambda n: n[0].update(current_term=2),
            lambda n: n[0].update(commit_idx=2),
            lambda n: n[0]['repair'].update(healed_requests=1),
            lambda n: n[1]['entries'][1].update(term=1),
            lambda n: n[2].update(last_applied_idx=2),
            lambda n: n[0]['entries'][1].update(bytes_hex='ff'),
        ):
            observed = nodes(root=True); change(observed)
            self.assertFalse(repair.root_valid(observed))

    def test_mutant_requires_exact_diagnostic_and_committed_conflict(self):
        reply = repair.DIAGNOSTIC+'\nReach illegal state.\n'
        observed = nodes(mutant=True)
        self.assertTrue(repair.terminal(reply, observed, 'mutant'))
        observed[0]['last_applied_idx'] = 1  # ACK boundary precedes apply_all.
        self.assertTrue(repair.terminal(reply, observed, 'mutant'))
        for wrong in ('Reach illegal state.\n', reply.replace('entry 2', 'entry 3'),
                      reply+'other: property violation: wrong\n', repair.DIAGNOSTIC):
            with self.assertRaises(common.CaseFailure): repair.terminal(wrong, observed, 'mutant')
        for change in (
            lambda n: n[0].update(commit_idx=1),
            lambda n: n[0]['entries'][1].update(term=2),
            lambda n: n[0]['entries'][1].update(bytes_hex='ff'),
            lambda n: n[1]['entries'][1].update(bytes_hex='00'),
            lambda n: n[0]['repair']['last_request'].update({'append.prev_log_idx': 1}),
            lambda n: n[0]['repair'].update(healed_requests=1),
            lambda n: n[0]['repair']['last_reply'].update({'appended.success': 0}),
            lambda n: n[0]['repair']['last_request'].update({'append.n_entries': 2}),
        ):
            changed = copy.deepcopy(observed); change(changed)
            with self.assertRaises(common.CaseFailure): repair.terminal(reply, changed, 'mutant')

    def test_healthy_requires_exact_repaired_logs_apply_and_rejections(self):
        self.assertTrue(repair.terminal('', nodes(), 'healthy'))
        observed = nodes(); observed[0]['last_applied_idx'] = 1
        self.assertFalse(repair.terminal('', observed, 'healthy'))
        observed = nodes(); observed[0]['repair']['rejected'] = 1
        with self.assertRaises(common.CaseFailure): repair.terminal('', observed, 'healthy')
        observed = nodes(); observed[0]['entries'][2]['bytes_hex'] = 'ff'
        self.assertFalse(repair.terminal('', observed, 'healthy'))

    def test_matrix_rejects_incomplete_ledger_bad_hash_cleanup_replay_and_root(self):
        accepted = lambda rows: repair.accepted_rows(rows, ('healthy', 'mutant'), ('full', 'pages'))
        rows = matrix()
        self.assertTrue(accepted(rows))
        self.assertFalse(accepted(rows[:-1]))
        self.assertFalse(accepted(rows+[rows[0]]))
        for change in (
            lambda r: r.update(status='pass'),
            lambda r: r.update(native='failed'),
            lambda r: r.update(root_before_repair=False),
            lambda r: r['root_observation'][0]['repair'].update(healed_requests=1),
            lambda r: r['search_edges'].pop(),
            lambda r: r['search_edges'][0].update(predecessor='wrong'),
            lambda r: r['search_edges'][0].update(reply='gettimeofday(0,0) = 0\nDetSim state/execution error: broken'),
            lambda r: r['search_edges'][0].update(source_verified=False),
            lambda r: r['search_counters'].update(partial_layer=True),
            lambda r: r['search_counters'].update(depth_cutoff=True),
            lambda r: r['search_counters'].update(unique_successors=6),
            lambda r: r.update(owned_pids_remaining=[123]),
            lambda r: r.update(tracer_exit=1),
            lambda r: r['replay_records'][0].update(successor='wrong'),
            lambda r: r['replay_records'][0].update(diagnostic='wrong'),
            lambda r: r.update(search_driver_sha256='bad'),
            lambda r: r.update(config_sha256='bad'),
        ):
            changed = copy.deepcopy(rows); change(changed[-1])
            self.assertFalse(accepted(changed))

    def test_native_matrix_still_requires_exact_variants_and_success(self):
        rows = [dict(variant=v, status='pass', native='pass') for v in ('healthy', 'mutant')]
        self.assertTrue(repair.accepted_rows(rows, ('healthy', 'mutant'), (), True))
        rows[-1]['native'] = 'failed'
        self.assertFalse(repair.accepted_rows(rows, ('healthy', 'mutant'), (), True))


if __name__ == '__main__':
    unittest.main()
