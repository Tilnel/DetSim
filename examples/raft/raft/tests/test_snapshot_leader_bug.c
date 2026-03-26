/*
 * Test case for snapshot loading bug that causes dual leader.
 * 
 * Bug description:
 * When a node loads a snapshot, raft_begin_load_snapshot() sets num_nodes to 1.
 * After raft_end_load_snapshot(), if raft_periodic() is called, the node thinks
 * it's the only node in the cluster and becomes leader via single-node bootstrap.
 * This can result in multiple leaders in the same term.
 * 
 * This test reproduces the scenario from the detsim trace where Node 0 loads
 * a snapshot from Node 1 (the leader), then incorrectly becomes leader itself.
 * 
 * FIX: After raft_begin_load_snapshot(), the user must restore membership
 * by calling raft_add_node() for each peer node before raft_end_load_snapshot().
 */

#include <stdbool.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "CuTest.h"

#include "raft.h"
#include "raft_log.h"
#include "raft_private.h"

static int __raft_persist_term(
    raft_server_t* raft,
    void *udata,
    raft_term_t term,
    int vote
    )
{
    return 0;
}

static int __raft_persist_vote(
    raft_server_t* raft,
    void *udata,
    int vote
    )
{
    return 0;
}

static int __raft_applylog(
    raft_server_t* raft,
    void *udata,
    raft_entry_t *ety,
    raft_index_t idx
    )
{
    return 0;
}

static int __raft_send_requestvote(raft_server_t* raft,
                            void* udata,
                            raft_node_t* node,
                            msg_requestvote_t* msg)
{
    return 0;
}

static int __raft_send_appendentries(raft_server_t* raft,
                              void* udata,
                              raft_node_t* node,
                              msg_appendentries_t* msg)
{
    return 0;
}

/* Helper to restore membership after snapshot load (the fix) */
static void restore_membership_after_snapshot(void *raft, int self_id)
{
    /* Re-add all peer nodes (they were removed from nodes array by begin_load_snapshot) */
    for (int i = 0; i < 3; i++) {
        if (i != self_id) {
            raft_add_node(raft, NULL, i, 0);
        }
    }
}

/* 
 * Test: Loading snapshot reduces num_nodes to 1, causing single-node bootstrap
 * WITHOUT the fix (to verify bug exists).
 */
void TestRaft_snapshot_load_causes_single_node_leader_bootstrap(CuTest * tc)
{
    raft_cbs_t funcs = {
        .send_appendentries = __raft_send_appendentries,
        .send_requestvote = __raft_send_requestvote,
        .persist_term = __raft_persist_term,
        .persist_vote = __raft_persist_vote,
        .applylog = __raft_applylog,
    };

    /* Setup: Create a 3-node cluster */
    void *r = raft_new();
    raft_set_callbacks(r, &funcs, NULL);
    raft_set_election_timeout(r, 1000);

    raft_add_node(r, NULL, 0, 1);  /* Node 0, is_self=1 */
    raft_add_node(r, NULL, 1, 0);  /* Node 1 */
    raft_add_node(r, NULL, 2, 0);  /* Node 2 */

    CuAssertIntEquals(tc, 3, raft_get_num_nodes(r));
    CuAssertTrue(tc, raft_is_follower(r));

    /* Load snapshot WITHOUT restoring membership (to show bug) */
    raft_begin_load_snapshot(r, 1, 2);
    /* NOT calling restore_membership_after_snapshot() */
    raft_end_load_snapshot(r);

    /* BUG: num_nodes is 1 after loading snapshot */
    CuAssertIntEquals(tc, 1, raft_get_num_nodes(r));

    /* BUG: raft_periodic causes single-node bootstrap */
    raft_periodic(r, 100);
    CuAssertTrue(tc, raft_is_leader(r));  /* Shows the bug */

    raft_free(r);
}

/*
 * Test: With the fix (restoring membership), dual leader is prevented.
 */
void TestRaft_snapshot_load_with_membership_restoration_prevents_dual_leader(CuTest * tc)
{
    raft_cbs_t funcs = {
        .send_appendentries = __raft_send_appendentries,
        .send_requestvote = __raft_send_requestvote,
        .persist_term = __raft_persist_term,
        .persist_vote = __raft_persist_vote,
        .applylog = __raft_applylog,
    };

    /* Create Node 1 as leader (simulating the trace leader) */
    void *node1 = raft_new();
    raft_set_callbacks(node1, &funcs, NULL);
    raft_add_node(node1, NULL, 0, 0);  /* Node 0 */
    raft_add_node(node1, NULL, 1, 1);  /* Node 1, self */
    raft_add_node(node1, NULL, 2, 0);  /* Node 2 */
    
    raft_set_current_term(node1, 1);
    raft_become_leader(node1);
    CuAssertTrue(tc, raft_is_leader(node1));
    CuAssertIntEquals(tc, 1, raft_get_current_term(node1));

    /* Create Node 0 as follower */
    void *node0 = raft_new();
    raft_set_callbacks(node0, &funcs, NULL);
    raft_add_node(node0, NULL, 0, 1);  /* Node 0, self */
    raft_add_node(node0, NULL, 1, 0);  /* Node 1 */
    raft_add_node(node0, NULL, 2, 0);  /* Node 2 */

    CuAssertTrue(tc, raft_is_follower(node0));
    CuAssertIntEquals(tc, 3, raft_get_num_nodes(node0));

    /* Node 0 loads snapshot with FIX: restore membership */
    raft_begin_load_snapshot(node0, 1, 2);
    restore_membership_after_snapshot(node0, 0);  /* THE FIX */
    raft_end_load_snapshot(node0);

    /* Verify: membership is restored */
    CuAssertIntEquals(tc, 3, raft_get_num_nodes(node0));

    /* Node 0 triggers periodic - should NOT become leader */
    raft_periodic(node0, 100);

    /* FIX VERIFIED: Node 0 remains follower */
    CuAssertTrue(tc, raft_is_follower(node0));
    
    /* Only Node 1 is leader */
    CuAssertTrue(tc, raft_is_leader(node1));
    CuAssertTrue(tc, !raft_is_leader(node0));

    printf("\n[FIX VERIFIED] After loading snapshot with membership restoration:\n");
    printf("  Node 0: %s (correctly stayed follower)\n", 
           raft_is_follower(node0) ? "follower" : "leader");
    printf("  Node 1: %s (original leader)\n",
           raft_is_leader(node1) ? "leader" : "follower");

    raft_free(node0);
    raft_free(node1);
}

/*
 * Test: With the fix, dual leader violation is prevented
 * 
 * This test verifies that when the fix is applied (restoring membership
 * after snapshot load), dual leader cannot occur.
 */
void TestRaft_snapshot_load_prevents_dual_leader_with_fix(CuTest * tc)
{
    raft_cbs_t funcs = {
        .send_appendentries = __raft_send_appendentries,
        .send_requestvote = __raft_send_requestvote,
        .persist_term = __raft_persist_term,
        .persist_vote = __raft_persist_vote,
        .applylog = __raft_applylog,
    };

    /* Create Node 1 as leader */
    void *node1 = raft_new();
    raft_set_callbacks(node1, &funcs, NULL);
    raft_add_node(node1, NULL, 0, 0);
    raft_add_node(node1, NULL, 1, 1);
    raft_add_node(node1, NULL, 2, 0);
    
    raft_set_current_term(node1, 1);
    raft_become_leader(node1);
    CuAssertTrue(tc, raft_is_leader(node1));

    /* Create Node 0 as follower */
    void *node0 = raft_new();
    raft_set_callbacks(node0, &funcs, NULL);
    raft_add_node(node0, NULL, 0, 1);
    raft_add_node(node0, NULL, 1, 0);
    raft_add_node(node0, NULL, 2, 0);

    /* WITH FIX: Node 0 loads snapshot and restores membership */
    raft_begin_load_snapshot(node0, 1, 2);
    restore_membership_after_snapshot(node0, 0);  /* THE FIX */
    raft_end_load_snapshot(node0);
    raft_periodic(node0, 100);

    /* FIX VERIFIED: Only Node 1 is leader */
    CuAssertTrue(tc, raft_is_leader(node1));
    CuAssertTrue(tc, !raft_is_leader(node0));

    raft_free(node0);
    raft_free(node1);
}
