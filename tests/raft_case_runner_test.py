"""Accept only the controlled denied-vote Raft mutant counterexample."""
import copy
from pathlib import Path
import sys
import struct
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "examples/cases"))
import raft_run


class MutantCounterexampleAcceptance(unittest.TestCase):
    def setUp(self):
        self.reply = ("willemt-raft.safety: property violation: multiple leaders in term 1\n"
                      "Reach illegal state.\n")
        self.nodes = [
            {"node": 0, "state": 3, "current_term": 1, "voted_for": 0},
            {"node": 1, "state": 3, "current_term": 1, "voted_for": 1},
            {"node": 2, "state": 1, "current_term": 1, "voted_for": 0},
        ]
        self.receipt = {"count": 1, "kind": 2, "from": 2, "to": 1,
                        "reply.vote_granted": 0}

    def accepts(self, reply=None, nodes=None, receipt=None):
        return raft_run.expected_mutant_violation(
            self.reply if reply is None else reply,
            self.nodes if nodes is None else nodes,
            self.receipt if receipt is None else receipt)

    def test_expected_discovery_and_replay(self):
        self.assertTrue(self.accepts())
        self.assertTrue(self.accepts(reply=self.reply.replace(
            "Reach illegal state.", "Stopped for illegal state. Searched for 16 sys_states")))
        # Node identity, rather than the order of inspection, identifies A/B/C.
        self.assertTrue(self.accepts(nodes=list(reversed(self.nodes))))

    def test_unrelated_or_missing_diagnostic(self):
        for diagnostic in ("", "different property failed", "multiple leaders in term 10",
                           "multiple leaders in term 2", "multiple leaders in term 1 extra"):
            with self.subTest(diagnostic=diagnostic):
                self.assertFalse(self.accepts(reply=(
                    "willemt-raft.safety: property violation: " + diagnostic + "\nReach illegal state.\n")))
        self.assertFalse(self.accepts(reply=self.reply.replace("Reach illegal state.", "")))

    def test_wrong_leader_or_term(self):
        for node in (0, 1):
            for field, value in (("state", 2), ("current_term", 0), ("current_term", 2)):
                with self.subTest(node=node, field=field, value=value):
                    nodes = copy.deepcopy(self.nodes)
                    nodes[node][field] = value
                    self.assertFalse(self.accepts(nodes=nodes))

    def test_missing_or_duplicate_logical_node(self):
        self.assertFalse(self.accepts(nodes=self.nodes[:2]))
        self.assertFalse(self.accepts(nodes=[self.nodes[0], self.nodes[0], self.nodes[2]]))

    def test_c_must_have_voted_for_a(self):
        for vote in (-1, 1):
            nodes = copy.deepcopy(self.nodes)
            nodes[2]["voted_for"] = vote
            self.assertFalse(self.accepts(nodes=nodes))

    def test_denied_vote_receipt_fields(self):
        for field, value in (("count", 0), ("kind", 1), ("from", 0), ("to", 0),
                             ("reply.vote_granted", 1)):
            with self.subTest(field=field):
                receipt = self.receipt | {field: value}
                self.assertFalse(self.accepts(receipt=receipt))
        for field in self.receipt:
            with self.subTest(missing=field):
                receipt = self.receipt.copy()
                del receipt[field]
                self.assertFalse(self.accepts(receipt=receipt))


class HealthySearchAcceptance(unittest.TestCase):
    def test_heartbeat_requires_completed_empty_receive(self):
        self.assertTrue(raft_run.leader_clock_ready(
            "si 0\nrecvfrom(3, 0x1234, 272, 64, (nil), NULL) = -11 (Resource temporarily unavailable)\n(ptmc) "))
        for reply in ("", "gettimeofday(0x1234, NULL) = 0\n", "sendto(3, 0x1234) = -11\n",
                      "recvfrom(3, 0x1234) = 272\n", "recvfrom(3, 0x1234) = -110\n"):
            with self.subTest(reply=reply):
                self.assertFalse(raft_run.leader_clock_ready(reply))

    @staticmethod
    def record(key, budget, schema=(0x4453534d, 2, 3)):
        payload = struct.pack("<III", *schema) + bytes(36) + struct.pack("<i?", budget, False)
        return struct.pack("<IQII", 0x53534545, key, len(payload), 0) + payload

    def test_budget_matches_checkpoint_instead_of_last_append(self):
        with tempfile.TemporaryDirectory(prefix="detsim-raft-budget-") as temporary:
            directory = Path(temporary)
            (directory / "sstate").mkdir()
            packed = directory / "sstate/packed.dat"
            packed.write_bytes(self.record(1, 2) + self.record(2, 0))
            self.assertEqual(raft_run.saved_budget(directory, "0000000000000001"), 2)
            self.assertEqual(raft_run.saved_budget(directory, "0000000000000002"), 0)
            with self.assertRaises(raft_run.common.CaseFailure):
                raft_run.saved_budget(directory, "0000000000000003")

    def test_budget_rejects_bad_schema_and_truncated_records(self):
        with tempfile.TemporaryDirectory(prefix="detsim-raft-budget-") as temporary:
            directory = Path(temporary)
            (directory / "sstate").mkdir()
            packed = directory / "sstate/packed.dat"
            for blob in (b"partial", self.record(1, 2)[:-1],
                         self.record(1, 2, schema=(0x4453534d, 2, 2))):
                with self.subTest(blob=blob):
                    packed.write_bytes(blob)
                    with self.assertRaises(raft_run.common.CaseFailure):
                        raft_run.saved_budget(directory, "0000000000000001")


if __name__ == "__main__":
    unittest.main()
