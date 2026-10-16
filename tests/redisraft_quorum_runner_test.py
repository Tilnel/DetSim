"""Strict terminal and matrix acceptance for the real-library quorum case."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'examples/cases'))
import redisraft_quorum_run as quorum
import run as common

class QuorumAcceptance(unittest.TestCase):
    def setUp(self):
        self.entry = dict(index=2, term=1, type=0, data_len=len(quorum.COMMAND), bytes_hex=quorum.COMMAND.hex())
        self.nodes = [dict(node=i, commit_idx=2 if i==0 else 0, last_applied_idx=2 if i==0 else 0,
                           entries=[self.entry] if i==0 else []) for i in range(3)]
        self.reply = 'sendto(3, 1, 1, 0, 1, 16) = 352\n'+quorum.DIAGNOSTIC+'\nReach illegal state.\n'

    def test_exact_mutant_diagnostic_and_copy_evidence(self):
        self.assertTrue(quorum.terminal(self.reply, self.nodes, 'mutant'))
        for reply in ('Reach illegal state.\n', self.reply.replace('2 lacks','3 lacks'),
                      self.reply.replace('Reach illegal state.', ''), self.reply+'other: property violation: wrong\n'):
            with self.subTest(reply=reply), self.assertRaises(common.CaseFailure):
                quorum.terminal(reply,self.nodes,'mutant')
        for key,value in (('term',2),('type',4),('bytes_hex','ff')):
            nodes=copy.deepcopy(self.nodes); nodes[0]['entries'][0][key]=value
            with self.assertRaises(common.CaseFailure): quorum.terminal(self.reply,nodes,'mutant')
        nodes=copy.deepcopy(self.nodes); nodes[1]['entries']=[self.entry]
        with self.assertRaises(common.CaseFailure): quorum.terminal(self.reply,nodes,'mutant')

    def test_healthy_requires_normal_apply_and_majority(self):
        with self.assertRaises(common.CaseFailure): quorum.terminal('',self.nodes,'healthy')
        nodes=copy.deepcopy(self.nodes); nodes[1]['entries']=[self.entry]
        self.assertTrue(quorum.terminal('',nodes,'healthy'))
        nodes[0]['last_applied_idx']=1
        self.assertFalse(quorum.terminal('',nodes,'healthy'))
        nodes=copy.deepcopy(self.nodes);nodes[0]['entries']=[]
        self.assertFalse(quorum.terminal('',nodes,'healthy'))

    def test_missing_duplicate_wrong_status_cleanup_and_replay_rejected(self):
        rows=[]
        for variant in ('healthy','mutant'):
            for backend in ('full','pages'):
                verdict='pass' if variant=='healthy' else 'violation'
                nodes=copy.deepcopy(self.nodes)
                if variant=='healthy': nodes[1]['entries']=[self.entry]
                reply=self.reply if variant=='mutant' else 'gettimeofday(1, 0) = 0\n'
                edge=dict(source_verified=True,actor_switch_verified=True,terminal=True,reply=reply,verdict=verdict,diagnostic=quorum.DIAGNOSTIC if variant=='mutant' else None, predecessor='root',successor='abc',observation=nodes,layer=0,node=0,preset=0)
                edges=[edge | dict(node=n,preset=p) for n in range(3) for p in range(2)]
                rows.append(dict(variant=variant,backend=backend,status=verdict,native='pass',tracer_exit=0,
                    owned_pids_before_close=[123,124,125,126],owned_pids_remaining=[],root_leader_matches=[1,0,0],
                    scenario_runner_sha256='a'*64,binary_sha256='b'*64,checker_sha256='c'*64,
                    choose_sha256='d'*64,config_sha256='e'*64,tracer_sha256='f'*64,root='root',root_before_NORMAL=True,
                    root_observation=[dict(node=i,commit_idx=0,entries=[dict(index=1,term=1,type=4,data_len=0,bytes_hex='')]) for i in range(3)],search_edges=edges,retained_sources=[],
                    search_counters=dict(attempted_actions=6,partial_layer=False,depth_cutoff=False,source_layers=1,
                        unique_successors=1,alias_actions=5,retained_sources=0,pruned_actions=5,pruned_distinct_children=0,beam_width=1),
                    terminal_edge=edge,replay='same_process_hash_verdict_observation_match_3x',
                    replay_records=[dict(predecessor='root',node=0,preset=0,diagnostic=edge['diagnostic'],successor='abc',observation=nodes,verdict=verdict) for _ in range(3)]))
        def accepted(value): return quorum.accepted_rows(value,('healthy','mutant'),('full','pages'))
        self.assertTrue(accepted(rows))
        self.assertFalse(accepted(rows[:-1]))
        self.assertFalse(accepted(rows+[rows[0]]))
        for key,value in (('status','pass'),('tracer_exit',1),('replay',''),('replay_records',[]),('native','failed')):
            changed=copy.deepcopy(rows);changed[-1][key]=value
            self.assertFalse(accepted(changed))
        for key,value in (('predecessor','wrong'),('node',1),('preset',1),('diagnostic','wrong'),('successor','wrong'),('observation',[]),('verdict','pass')):
            changed=copy.deepcopy(rows);changed[-1]['replay_records'][0][key]=value
            self.assertFalse(accepted(changed))

        for mutate in (
            lambda row: row.update(root_leader_matches=[1,1,0]),
            lambda row: row.update(owned_pids_before_close=[True,124,125,126]),
            lambda row: row['search_edges'][0].update(reply='gettimeofday(1, 0) = 0\nDetSim state/execution error: broken'),
            lambda row: row.update(scenario_runner_sha256='wrong'),
            lambda row: row['search_edges'][0].update(source_verified=False),
            lambda row: row['search_edges'][0].update(actor_switch_verified=False),
            lambda row: row['search_edges'][0].update(reply='si 0\n(ptmc) '),
            lambda row: row['search_counters'].update(unique_successors=6),
            lambda row: row['root_observation'][0].update(commit_idx=2),
            lambda row: row['search_edges'].pop(),
            lambda row: row['search_edges'][0].update(predecessor='wrong'),
            lambda row: row['search_edges'][0].update(reply='DetSim state/execution error: broken'),
            lambda row: row['terminal_edge'].update(terminal=False),
            lambda row: row['terminal_edge'].update(layer=1),
            lambda row: row['search_counters'].update(depth_cutoff=True),
            lambda row: row['search_counters'].update(partial_layer=True),
            lambda row: row.update(owned_pids_remaining=[123]),
        ):
            changed=copy.deepcopy(rows);mutate(changed[-1])
            self.assertFalse(accepted(changed))

if __name__=='__main__': unittest.main()
