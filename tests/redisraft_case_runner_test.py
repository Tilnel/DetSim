"""Reject unrelated or pre-vote failures in the RedisLabs library case."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'examples/cases'))
import redisraft_run

class DeniedActualVote(unittest.TestCase):
    def setUp(self):
        self.reply = ('redisraft.safety: property violation: multiple leaders in term 1\n'
                      'Reach illegal state.\n')
        self.nodes = [dict(node=0,state=4,current_term=1,voted_for=0),
                      dict(node=1,state=4,current_term=1,voted_for=1),
                      dict(node=2,state=1,current_term=1,voted_for=0)]
        self.receipt = {'count':1,'kind':2,'from':2,'to':1,'reply.vote_granted':0,
                        'reply.prevote':0,'reply.request_term':1}

    def accepts(self, reply=None, nodes=None, receipt=None):
        return redisraft_run.expected_mutant_violation(
            self.reply if reply is None else reply,
            self.nodes if nodes is None else nodes,
            self.receipt if receipt is None else receipt)

    def test_only_native_proven_denials(self):
        for sender in (0,2):
            self.assertTrue(self.accepts(receipt=self.receipt | {'from':sender}))
        self.assertTrue(self.accepts(nodes=list(reversed(self.nodes))))
        self.assertTrue(self.accepts(reply=self.reply.replace('Reach illegal state.',
                                                            'Stopped for illegal state.')))

    def test_other_property_or_term_is_not_success(self):
        for replacement in ('', 'assertion failed', 'multiple leaders in term 10',
                            'multiple leaders in term 1 extra'):
            self.assertFalse(self.accepts(reply='redisraft.safety: property violation: '+
                                           replacement+'\nReach illegal state.\n'))
        self.assertFalse(self.accepts(reply=self.reply.replace('Reach illegal state.', '')))

    def test_role_term_and_logical_identity(self):
        for node in (0,1):
            for field,value in (('state',3),('current_term',0),('current_term',2)):
                nodes=copy.deepcopy(self.nodes);nodes[node][field]=value
                self.assertFalse(self.accepts(nodes=nodes))
        self.assertFalse(self.accepts(nodes=self.nodes[:2]))
        self.assertFalse(self.accepts(nodes=[self.nodes[0],self.nodes[0],self.nodes[2]]))
        nodes=copy.deepcopy(self.nodes);nodes[2]['voted_for']=1
        self.assertFalse(self.accepts(nodes=nodes))

    def test_prevote_wrong_request_or_granted_reply_is_rejected(self):
        for field,value in (('count',0),('kind',1),('from',1),('to',0),
                            ('reply.vote_granted',1),('reply.prevote',1),('reply.request_term',2)):
            with self.subTest(field=field):
                self.assertFalse(self.accepts(receipt=self.receipt | {field:value}))
        for field in self.receipt:
            receipt=self.receipt.copy();del receipt[field]
            self.assertFalse(self.accepts(receipt=receipt))

if __name__ == '__main__':
    unittest.main()
