#!/usr/bin/env python3
"""Bounded case-search contracts without tracer processes."""
import copy
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import redisraft_full_run as runner
from native_result_test import records

ROOT="0000000000000000"


class GraphSession:
    def __init__(self):
        self.state=ROOT;self.node=0;self.slot=0;self.commands=[]
        self.graph={(ROOT,3,0,0):("0000000000000001",'write(1, 0x123"{"v":1,"event":"connected"}\\010", 28) = 28'),
                    (ROOT,3,1,0):("0000000000000002","stepped")}

    def current_hash(self):return self.state

    def send(self,command):
        self.commands.append(command)
        if command.startswith("load "):
            self.state=command.split()[1];return "Loaded state "+self.state
        if command.startswith("sw "):self.node=int(command.split()[1]);return "switched"
        if command.startswith("thread "):
            self.slot=int(command.split()[1]);return "Switched to thread %d (TID=99999)"%self.slot
        if command=="thread":
            slots=sorted({slot for state,node,slot,choice in self.graph if state==self.state and node==self.node})
            if not slots:slots=[0]
            return "Threads in process %d (pid=99999):\n"%self.node+"".join(
                " [%d] TID %d state=%s\n"%(slot,slot+1,"runnable" if (self.state,self.node,slot,0) in self.graph else "waiting") for slot in slots)
        if command in ("si 0","si 1"):
            self.state,reply=self.graph[(self.state,self.node,self.slot,int(command.split()[1]))];return reply
        if command=="info current":return "00000000deadbeef => "+self.state+" (wrong first incoming action)"
        raise RuntimeError("unexpected command "+command)


class SearchTest(unittest.TestCase):
    def test_explicit_time_profile_enumerates_both_presets_and_aliases_are_not_branches(self):
        session=GraphSession();session.normal_choices=(0,1)
        self.assertEqual(runner.search_actions(session),[(3,0,0),(3,0,1),(3,1,0),(3,1,1)])
        session.graph={(ROOT,3,0,0):("0000000000000001","exit_group(0) = 0"),
                       (ROOT,3,0,1):("0000000000000001","exit_group(0) = 0")}
        result=runner.guided_search(session,ROOT,{"observations":records(False),"client_exit":None},"healthy",2,10)
        self.assertEqual(result["termination"],"degenerate_single_path")
        self.assertFalse(result["branching_observed"])
        self.assertEqual(result["duplicate_successor_edges"],1)

    def test_guide_excludes_nonidle_worker_timeout_and_prefers_positive_preset(self):
        candidates=[{"action":action} for action in ((1,1,-2),(1,0,0),(1,0,1),(2,0,0),(2,0,1))]
        self.assertEqual(runner.select_continuation(candidates,None)["action"],(1,0,1))
        self.assertEqual(runner.select_continuation(candidates,(1,0))["action"],(2,0,1))
        timed=[{"action":action} for action in ((0,0,-2),(2,0,-2))]
        self.assertEqual(runner.select_continuation(timed,(0,0))["action"],(2,0,-2))

    def test_guide_admits_locally_idle_timeout_beside_normal_peer(self):
        candidates=[{"action":action} for action in ((0,0,-2),(1,0,0),(1,0,1),(1,1,-2))]
        self.assertEqual(runner.select_continuation(candidates,None)["action"],(0,0,-2))
        self.assertEqual(runner.select_continuation(candidates,(0,0))["action"],(1,0,1))
        self.assertEqual(runner.select_continuation(candidates,(1,0))["action"],(0,0,-2))

    def test_nonidle_worker_timeout_is_executed_as_sibling_but_not_retained(self):
        class WithTimeout(GraphSession):
            def __init__(self):
                super().__init__()
                self.graph={(ROOT,0,0,0):("0000000000000001","stepped"),
                            (ROOT,0,1,-2):("0000000000000002","Timeout completed."),
                            (ROOT,1,0,-2):("0000000000000003","Timeout completed.")}
            def send(self,command):
                if command=="thread":
                    slots=sorted({slot for state,node,slot,choice in self.graph if state==self.state and node==self.node}) or [0]
                    reply="Threads in process %d (pid=999):\n"%self.node
                    for slot in slots:
                        normal=(self.state,self.node,slot,0) in self.graph
                        timed=(self.state,self.node,slot,-2) in self.graph
                        reply+=" [%d] TID %d state=%s%s\n"%(slot,slot+1,"runnable" if normal else "waiting",
                                                                           " deadline=1.000000000" if timed else "")
                    return reply
                if command=="si timeout":
                    self.state,reply=self.graph[(self.state,self.node,self.slot,-2)];return reply
                return super().send(command)
        session=WithTimeout();events=[]
        candidates=runner.search_layer(session,ROOT,{"observations":[],"client_exit":None},events)
        self.assertEqual([candidate["action"] for candidate in candidates],[(0,0,0),(0,1,-2),(1,0,-2)])
        self.assertTrue(all(event["executed"] for event in events))
        self.assertEqual(runner.select_continuation(candidates,(0,0))["action"],(1,0,-2))

    def test_layer_restores_each_candidate_and_isolates_observations(self):
        session=GraphSession();events=[];driver={"observations":[],"client_exit":None}
        layer=runner.search_layer(session,ROOT,driver,events)
        self.assertEqual({candidate["edge"]["successor"] for candidate in layer},
                         {"0000000000000001","0000000000000002"})
        self.assertEqual(layer[0]["driver"]["observations"],[{"v":1,"event":"connected"}])
        self.assertEqual(layer[1]["driver"]["observations"],[])
        self.assertEqual(driver["observations"],[])
        self.assertTrue(all(edge["predecessor"]==ROOT for edge in events))
        self.assertNotIn("deadbeef",str(events))

    def test_depth_cutoff_never_passes_and_records_pruning(self):
        session=GraphSession()
        result=runner.guided_search(session,ROOT,{"observations":[],"client_exit":None},"healthy",1,10)
        self.assertEqual(result["status"],"incomplete")
        self.assertEqual(result["termination"],"depth_cutoff")
        self.assertEqual(result["pruned_successors"],1)
        self.assertTrue(result["branching_observed"])

    def test_wrong_property_and_incomplete_healthy_reject(self):
        driver={"observations":records(False),"client_exit":0}
        self.assertEqual(runner.model_outcome(driver,"healthy","pass",""),"pass")
        for bad in (driver["observations"][:-2]+driver["observations"][-1:],
                    driver["observations"][:-1]):
            with self.assertRaises(RuntimeError):runner.model_outcome({"observations":bad,"client_exit":0},"healthy","pass","")
        mutant=records(True);mutant=[x for x in mutant if x.get("event")!="violation" and not (x.get("node")==3 and x.get("key")=="beta")]
        with self.assertRaises(RuntimeError):runner.model_outcome({"observations":mutant,"client_exit":None},"mutant","violation","other property")
        self.assertEqual(runner.model_outcome({"observations":mutant,"client_exit":None},"mutant","violation",
                         "acknowledged local value missing at node 3 key alpha"),"violation")

    def test_terminal_completion_replays_three_times(self):
        session=GraphSession();session.graph[(ROOT,3,0,0)]=( "0000000000000001","exit_group(0) = 0")
        result=runner.guided_search(session,ROOT,{"observations":records(False),"client_exit":None},"healthy",2,10)
        self.assertEqual(result["status"],"pass",result)
        self.assertEqual(len(result["replays"]),3)
        self.assertTrue(result["replay_verified"])
        self.assertEqual(result["path"][-1]["target"],result["witness"]["target"])

    def test_degenerate_terminal_and_wrong_property_sibling_do_not_pass(self):
        session=GraphSession();session.graph={(ROOT,3,0,0):("0000000000000001","exit_group(0) = 0")}
        driver={"observations":records(False),"client_exit":None}
        result=runner.guided_search(session,ROOT,driver,"healthy",2,10)
        self.assertEqual(result["termination"],"degenerate_single_path")
        self.assertEqual(result["status"],"incomplete")
        session.graph[(ROOT,3,1,0)]=( "0000000000000002","cases.redisraft-full: property violation: wrong property\nReach illegal state")
        result=runner.guided_search(session,ROOT,driver,"healthy",2,10)
        self.assertEqual(result["status"],"failed")
        self.assertFalse(result["replay_verified"])

    def test_round_robin_selection_is_variant_independent(self):
        paths=[]
        for variant in ("healthy","mutant"):
            session=GraphSession()
            session.graph.update({("0000000000000001",3,0,0):("0000000000000003","stepped"),
                                  ("0000000000000001",3,1,0):("0000000000000004","stepped")})
            result=runner.guided_search(session,ROOT,{"observations":[],"client_exit":None},variant,2,10)
            paths.append([edge["action"] for edge in result["path"]])
        self.assertEqual(paths[0],[[3,0,0],[3,1,0]])
        self.assertEqual(paths[0],paths[1])

    def test_partial_layer_timeout_preserves_attempts_and_is_not_green(self):
        class Interrupted(GraphSession):
            def send(self,command):
                if command=="si 0" and self.slot==1:raise TimeoutError("fixture wall cutoff")
                return super().send(command)
        result=runner.guided_search(Interrupted(),ROOT,{"observations":[],"client_exit":None},"healthy",2,10)
        self.assertEqual(result["status"],"timeout")
        self.assertTrue(result["partial_last_layer"])
        self.assertEqual(result["attempted_edges"],2)
        self.assertEqual(result["unique_successors"],1)

    def test_replay_changed_hash_is_rejected(self):
        session=GraphSession();driver={"observations":records(False),"client_exit":None}
        session.graph[(ROOT,3,0,0)]=( "0000000000000001","exit_group(0) = 0")
        candidate=runner.search_layer(session,ROOT,driver,[])[0]
        session.graph[(ROOT,3,0,0)]=( "0000000000000009","exit_group(0) = 0")
        with self.assertRaisesRegex(RuntimeError,"replay changed"):
            runner.replay_candidate(session,ROOT,driver,candidate,"healthy")

    def test_retained_cycle_is_inconclusive(self):
        session=GraphSession();session.graph[(ROOT,3,0,0)]=(ROOT,"stepped")
        result=runner.guided_search(session,ROOT,{"observations":[],"client_exit":None},"healthy",20,10)
        self.assertEqual(result["status"],"incomplete")
        self.assertEqual(result["termination"],"retained_path_cycle")
        self.assertFalse(result["replay_verified"])

    def test_unknown_outcome_or_controller_choice_fails(self):
        with self.assertRaisesRegex(RuntimeError,"unknown product verdict"):
            runner.model_outcome({"observations":[],"client_exit":None},"healthy","unavailable","")
        with self.assertRaisesRegex(RuntimeError,"unknown controller action"):
            runner.control_step(GraphSession(),3,0,[],[""]*4,[],choice=1)

    def test_candidates_include_only_saved_finite_timeout_and_normal(self):
        class Timed(GraphSession):
            def send(self,command):
                if command=="thread" and self.node==0:
                    return ("Threads in process 0 (pid=999):\n"
                            " [0] TID 1 state=waiting\n"
                            " [1] TID 2 state=waiting deadline=1.000000002\n"
                            " [2] TID 3 state=exited\n")
                return super().send(command)
        self.assertEqual(runner.search_actions(Timed()),[(0,1,-2),(3,0,0),(3,1,0)])

    def test_late_third_replay_cannot_pass_wall_budget(self):
        clock={"now":0}
        class LateThirdReplay(GraphSession):
            def __init__(self):
                super().__init__();self.started=0;self.timeout=100;self.terminal_calls=0
                self.graph[(ROOT,3,0,0)]=( "0000000000000001","exit_group(0) = 0")
            def send(self,command):
                reply=super().send(command)
                if command=="si 0" and self.slot==0:
                    self.terminal_calls+=1
                    if self.terminal_calls==4:clock["now"]=11
                return reply
        session=LateThirdReplay()
        with patch.object(runner.time,"monotonic",side_effect=lambda:clock["now"]):
            result=runner.guided_search(session,ROOT,{"observations":records(False),"client_exit":None},"healthy",2,10)
        self.assertEqual(result["status"],"timeout",result)
        self.assertEqual(result["termination"],"wall_cutoff")
        self.assertTrue(result["wall_cutoff"])
        self.assertGreaterEqual(result["elapsed_seconds"],11)
        self.assertFalse(result["replay_verified"])
        self.assertEqual(session.timeout,100)

    def test_matrix_requires_rooted_contiguous_actual_path(self):
        for mutation in ("root","bad_hash","empty_path","broken_chain","edge_index","last_witness","prefix_ack","late"):
            rows=self.matrix_rows();row=rows[1];search=row["search"]
            if mutation=="root":search["root"]="0000000000000099"
            elif mutation=="bad_hash":row["prefix"]="not-a-hash";search["root"]=row["prefix"]
            elif mutation=="empty_path":search["path"]=[]
            elif mutation=="broken_chain":search["path"][0]["source"]="0000000000000099"
            elif mutation=="edge_index":search["path"][0]["edge_index"]=1
            elif mutation=="last_witness":search["path"][0]["target"]="0000000000000002"
            elif mutation=="prefix_ack":row["observations"].insert(0,{"event":"ack"})
            elif mutation=="late":search["elapsed_seconds"]=11
            with self.subTest(mutation=mutation):
                self.assertFalse(runner.matrix_acceptance(rows,("healthy","mutant"),("full","pages"))["model_matrix_complete"])

    def test_scoped_command_budget_is_restored_after_success(self):
        class BudgetSession(GraphSession):
            def __init__(self):
                super().__init__();self.started=0;self.timeout=100;self.seen=[]
                self.graph[(ROOT,3,0,0)]=( "0000000000000001","exit_group(0) = 0")
            def send(self,command):
                self.seen.append(self.timeout)
                return super().send(command)
        session=BudgetSession()
        with patch.object(runner.time,"monotonic",return_value=0):
            result=runner.guided_search(session,ROOT,{"observations":records(False),"client_exit":None},"healthy",2,10)
        self.assertEqual(result["status"],"pass")
        self.assertTrue(session.seen and all(value==10 for value in session.seen))
        self.assertEqual(session.timeout,100)
        self.assertNotIn("send",vars(session))
        self.assertNotIn("current_hash",vars(session))

    def test_valid_two_layer_path_and_internal_chain_break(self):
        rows=self.matrix_rows();search=rows[1]["search"];middle="0000000000000003"
        terminal=search["edges"]
        for edge in terminal:edge["predecessor"]=middle;edge["layer"]=1
        first={"predecessor":ROOT,"successor":middle,"node":0,"thread":0,"choice":0,"executed":True,
               "selected":True,"status":"completed","layer":0,"observations":[],"client_exit":None}
        sibling=dict(first,successor="0000000000000009",node=1,selected=False)
        search["edges"]=[first,sibling]+terminal
        search["path"][0]["source"]=middle;search["path"][0]["edge_index"]=2
        search["path"].insert(0,{"source":ROOT,"target":middle,"action":[0,0,0],"edge_index":0})
        search["witness"]["source"]=middle
        for replay in search["replays"]:replay["source"]=middle
        search.update(expanded_layers=2,attempted_edges=4);search["bounds"]["depth"]=2
        self.assertTrue(runner.matrix_acceptance(rows,("healthy","mutant"),("full","pages"))["passed"])
        search["path"][1]["source"]="0000000000000099"
        self.assertFalse(runner.matrix_acceptance(rows,("healthy","mutant"),("full","pages"))["passed"])

    def matrix_rows(self):
        rows=[]
        for variant in ("healthy","mutant"):
            status="pass" if variant=="healthy" else "violation"
            rows.append({"variant":variant,"backend":"native","status":status,"cleanup":"pass","NP":4,
                         "observations":records(variant=="mutant"),"client_exit":0 if variant=="healthy" else 42,
                         "checker_exit":0 if variant=="healthy" else 1,"server_exits":[0,0,0],
                         "checker_output":"NATIVE_CHECK verdict=0 diagnostic=" if variant=="healthy" else
                         "NATIVE_CHECK verdict=1 diagnostic=acknowledged local value missing at node 3 key alpha"})
            prefix=[{"v":1,"event":"before_set","key":"alpha"}]
            observed=prefix+records(variant=="mutant")
            if variant=="mutant":observed=[x for x in observed if x["event"]!="violation" and not (x.get("node")==3 and x.get("key")=="beta")]
            diagnostic="" if variant=="healthy" else "acknowledged local value missing at node 3 key alpha"
            witness={"source":ROOT,"target":"0000000000000001","action":[3,0,0],"diagnostic":diagnostic,
                     "driver":{"observations":observed,"client_exit":0 if variant=="healthy" else None}}
            search={"status":status,"branching_observed":True,"replay_verified":True,"beam_width":1,
                    "partial_last_layer":False,"expanded_layers":1,"attempted_edges":2,"witness":witness,
                    "selection":runner.GUIDE_POLICY,"normal_choices":[0,1],
                    "root":ROOT,"root_observations":prefix,"elapsed_seconds":1,"wall_cutoff":False,
                    "bounds":{"depth":1,"wall_seconds":10,"effective_wall_seconds":10},
                    "path":[{"source":ROOT,"target":witness["target"],"action":[3,0,0],"edge_index":0}],
                    "edges":[{"predecessor":ROOT,"successor":"0000000000000001","node":3,"thread":0,"choice":0,
                              "executed":True,"selected":True,"status":"completed","layer":0,
                              "observations":observed[len(prefix):],"client_exit":witness["driver"]["client_exit"]},
                             {"predecessor":ROOT,"successor":"0000000000000002","executed":True,"status":"completed","layer":0,
                              "node":3,"thread":1,"choice":0,"observations":[],"client_exit":None}],
                    "replays":[{"source":ROOT,"target":witness["target"],"action":witness["action"],"diagnostic":diagnostic,
                                "verdict":status,"observations":observed,"client_exit":witness["driver"]["client_exit"]} for _ in range(3)],
                    "termination":"healthy_completion" if variant=="healthy" else "exact_violation"}
            for backend in ("full","pages"):
                rows.append({"variant":variant,"backend":backend,"status":status,"cleanup":"pass","NP":4,"tracer_exit":0,
                             "search":copy.deepcopy(search),"prefix":ROOT,"observations":prefix})
        for row in rows:
            row.update({name:"0"*64 for name in ("module_sha256","client_sha256","checker_sha256","server_sha256","runner_sha256",
                                                 "config_sha256","tracer_sha256")})
            row["sources"]={"checker.cpp":"0"*64,"time_choice.cpp":"0"*64,"common_runner.py":"0"*64}
            row["time_policy"]=copy.deepcopy(runner.CLOCK_PROFILE)|{"plugin_sha256":"0"*64,"source_sha256":"0"*64}
        return rows

    def test_missing_duplicate_and_partial_matrix_never_model_complete(self):
        rows=self.matrix_rows()
        self.assertTrue(runner.matrix_acceptance(rows,("healthy","mutant"),("full","pages"))["model_matrix_complete"])
        for altered in (rows[:-1],rows+[copy.deepcopy(rows[0])]):
            self.assertFalse(runner.matrix_acceptance(altered,("healthy","mutant"),("full","pages"))["passed"])
        for field in ("replays","edges"):
            altered=copy.deepcopy(rows);altered[1]["search"][field]=[]
            self.assertFalse(runner.matrix_acceptance(altered,("healthy","mutant"),("full","pages"))["passed"])
        for field,value in (("selection","old guide"),("normal_choices",[0])):
            altered=copy.deepcopy(rows);altered[1]["search"][field]=value
            self.assertFalse(runner.matrix_acceptance(altered,("healthy","mutant"),("full","pages"))["passed"])
        altered=copy.deepcopy(rows);altered[0]["time_policy"].pop("prefix_timeout_policy")
        self.assertFalse(runner.matrix_acceptance(altered,("healthy","mutant"),("full","pages"))["passed"])
        altered=copy.deepcopy(rows);altered[1]["time_policy"]["plugin_sha256"]="1"*64
        self.assertFalse(runner.matrix_acceptance(altered,("healthy","mutant"),("full","pages"))["passed"])
        altered=copy.deepcopy(rows);altered[1]["module_sha256"]="1"*64
        self.assertFalse(runner.matrix_acceptance(altered,("healthy","mutant"),("full","pages"))["passed"])
        altered=copy.deepcopy(rows);altered[0]["sources"].pop("common_runner.py")
        self.assertFalse(runner.matrix_acceptance(altered,("healthy","mutant"),("full","pages"))["passed"])
        altered=copy.deepcopy(rows);altered[1]["sources"]["common_runner.py"]="1"*64
        self.assertFalse(runner.matrix_acceptance(altered,("healthy","mutant"),("full","pages"))["passed"])
        altered=copy.deepcopy(rows);altered[1]["search"]["replays"][1]["target"]="wrong"
        self.assertFalse(runner.matrix_acceptance(altered,("healthy","mutant"),("full","pages"))["passed"])
        partial=[x for x in rows if x["variant"]=="healthy"]
        result=runner.matrix_acceptance(partial,("healthy",),("full","pages"))
        self.assertTrue(result["requested_matrix_complete"])
        self.assertFalse(result["model_matrix_complete"])
        self.assertTrue(result["passed"])


if __name__=="__main__":unittest.main()
