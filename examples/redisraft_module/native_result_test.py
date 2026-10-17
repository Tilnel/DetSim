#!/usr/bin/env python3
"""Acceptance regressions; no servers or external builds required."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from redisraft_module import run as runner


def records(mutant=False):
    rows=[{"event":"ack","key":key,"value":value,"index":index}
          for key,value,index in (("alpha","17",1),("beta","29",2))]
    rows += [{"event":"progress","node":node,"commit":2,"applied":2} for node in range(1,4)]
    rows += [{"event":"local","node":node,"key":key,"present":not (mutant and node==3 and key=="alpha"),
              "matches":not (mutant and node==3 and key=="alpha")}
             for node in range(1,4) for key in ("alpha","beta")]
    return rows+[{"event":"violation" if mutant else "done"}]


class NativeResultTest(unittest.TestCase):
    def test_exact_fixtures(self):
        for mutant in (False,True):
            runner.native_result(records(mutant),42 if mutant else 0,"mutant" if mutant else "healthy")

    def test_missing_duplicate_and_unknown_cells_reject(self):
        for mutant in (False,True):
            original=records(mutant)
            local=[i for i,row in enumerate(original) if row["event"]=="local"]
            for index in local:
                for replacement in (None,copy.deepcopy(original[local[1] if index==local[0] else local[0]]),
                                    dict(original[index],node=4),dict(original[index],key="gamma")):
                    rows=copy.deepcopy(original)
                    if replacement is None: del rows[index]
                    else: rows[index]=replacement
                    with self.subTest(mutant=mutant,index=index,replacement=replacement):
                        with self.assertRaises(RuntimeError):
                            runner.native_result(rows,42 if mutant else 0,"mutant" if mutant else "healthy")

    def test_presence_and_matches_are_both_required(self):
        for mutant in (False,True):
            original=records(mutant)
            for index,row in enumerate(original):
                if row["event"]!="local": continue
                for field in ("present","matches"):
                    rows=copy.deepcopy(original);rows[index][field]=not rows[index][field]
                    with self.subTest(mutant=mutant,index=index,field=field):
                        with self.assertRaises(RuntimeError):
                            runner.native_result(rows,42 if mutant else 0,"mutant" if mutant else "healthy")


if __name__=="__main__": unittest.main()
