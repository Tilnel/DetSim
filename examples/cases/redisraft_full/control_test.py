#!/usr/bin/env python3
"""Public CLI controller tests; no tracer or workload processes required."""
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import redisraft_full_run as runner


class Session:
    def __init__(self):
        self.node=0;self.slot=0;self.commands=[];self.edges=0;self.reply="stepped"
        self.slots={0:[0,1,2],1:[0,1],2:[0],3:[0]}

    def send(self,command):
        self.commands.append(command)
        if command.startswith("sw "): self.node=int(command.split()[1])
        elif command=="thread":
            return "Threads in process %d (pid=98765):\n"%self.node+"".join(
                "%s [%d] TID %d %s\n"%("*" if slot==self.slot else " ",slot,100+slot,"(main)" if slot==0 else "")
                for slot in self.slots[self.node])
        elif command.startswith("thread "):
            self.slot=int(command.split()[1]);return "Switched to thread %d (TID=99999)"%self.slot
        elif command=="si 0":
            if "is blocked (pending syscall). Cannot step." not in self.reply:self.edges+=1
            return self.reply
        return ""

    def current_hash(self):return "%016x"%self.edges


class ControllerTest(unittest.TestCase):
    def test_all_logical_thread_slots_once_per_round(self):
        session=Session();events=[];last=[""]*4;observed=[]
        runner.actor_round(session,events,last,observed)
        self.assertEqual([(e["node"],e["thread"],e["choice"]) for e in events],
                         [(0,0,0),(0,1,0),(0,2,0),(1,0,0),(1,1,0),(2,0,0),(3,0,0)])
        for index,event in enumerate(events):
            self.assertTrue(event["executed"])
            self.assertEqual(event["status"],"completed")
            self.assertEqual(event["predecessor"],"%016x"%index)
            self.assertEqual(event["successor"],"%016x"%(index+1))
        self.assertNotIn("TID",json.dumps([(e["node"],e["thread"]) for e in events]))
        self.assertEqual(session.commands.count("si 0"),7)
        for index,command in enumerate(session.commands):
            if command=="si 0":self.assertTrue(session.commands[index-1].startswith("thread "))
        session.slots[2].append(1)
        runner.actor_round(session,events,last,observed)
        self.assertEqual([(e["node"],e["thread"]) for e in events[-8:]],
                         [(0,0),(0,1),(0,2),(1,0),(1,1),(2,0),(2,1),(3,0)])

    def test_blocked_attempt_is_not_completed_edge(self):
        session=Session();session.reply="Thread 1 is blocked (pending syscall). Cannot step."
        events=[]
        runner.control_step(session,0,1,events,[""]*4,[])
        self.assertEqual(session.edges,0)
        self.assertFalse(events[0]["executed"])
        self.assertEqual(events[0]["status"],"blocked")
        self.assertNotIn("successor",events[0])
        self.assertEqual(events[0]["observed_hash"],events[0]["predecessor"])

    def test_blocked_attempt_requires_unchanged_hash(self):
        class ChangedBlockedSession(Session):
            def send(self,command):
                reply=super().send(command)
                if command=="si 0":self.edges+=1
                return reply
        session=ChangedBlockedSession();session.reply="Thread 1 is blocked (pending syscall). Cannot step."
        with self.assertRaisesRegex(RuntimeError,"blocked.*hash"):
            runner.control_step(session,0,1,[],[""]*4,[])

    def test_assertion_stops_before_another_step(self):
        session=Session();session.reply='write(2, 0x123"client: client.c:90: main: Assertion `r.kind == + failed", 128) = 128'
        events=[]
        with self.assertRaises(runner.ClientAssertion):
            runner.control_step(session,3,0,events,[""]*4,[])
        self.assertEqual(session.commands.count("si 0"),1)
        self.assertIn("successor",events[0])
        self.assertIn("Assertion",events[0]["reply"])

    def test_model_assertion_is_failed_even_with_unsupported_in_log(self):
        class ModelSession(Session):
            def __init__(self,tracer,tracees,checker,logs,directory,backend,timeout):
                super().__init__();directory.mkdir()
                self.directory=directory;self.proc=SimpleNamespace(pid=99999990)
                self.log=directory/"transcript.log"
                self.log.write_text("DetSim state/execution error: unsupported syscall tgkill\n")
                (directory/"config.json").write_text("{}")

            def send(self,command=None):
                if command=="si 0":
                    self.reply="Assertion failed" if self.node==3 else "Ready to accept connections"
                return super().send(command) if command is not None else ""

            def close(self,timeout):return 0

        def saved_path(path):
            if str(path)=="/proc/99999990/task/99999990/children":
                return SimpleNamespace(read_text=lambda:"99999991 99999992 99999993 99999994")
            return Path(path)

        with tempfile.TemporaryDirectory() as directory, patch.object(runner,"FullSession",ModelSession), \
                patch.object(runner.common,"digest",return_value="hash"), patch.object(runner,"Path",side_effect=saved_path):
            row=runner.model_control(Path(directory),Path("tracer"),Path("module"),Path("client"),Path("checker"),
                                     "healthy","pages",10,2)
            self.assertEqual(row["status"],"failed")
            self.assertIn("client assertion",row["diagnostic"])
            events=json.loads((Path(row["artifacts"])/"control-events.json").read_text())
            self.assertEqual(events[-1]["node"],3)
            self.assertIn("successor",events[-1])
            self.assertEqual(row["cleanup"],"pass")

    def test_before_set_stops_round(self):
        session=Session()
        session.reply='write(1, 0x123"{"v":1,"event":"before_set","key":"alpha"}\\010", 48) = 48'
        events=[];observed=[]
        runner.actor_round(session,events,[""]*4,observed)
        self.assertEqual(observed[-1]["event"],"before_set")
        self.assertEqual(events[-1]["node"],3)

    def test_malformed_thread_listing_rejects(self):
        session=Session();session.slots[1]=[]
        with self.assertRaises(RuntimeError):runner.actor_round(session,[],[""]*4,[])


if __name__=="__main__":unittest.main()
