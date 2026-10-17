/* Real three-member Raft suffix repair, without crashes or compaction. */
#define RAFT_FIXTURE_ADAPTER_ONLY
#include "redisraft_fixture.c"

enum { REPAIR_OPEN, REPAIR_ISOLATED, REPAIR_HEALED };
struct repair_state {
    int phase;
    unsigned long dropped, healed_requests, requests, replies, rejected;
    struct packet last_dropped, last_request, last_reply;
};
/* Saved guest memory: routing decisions and exact callback receipts. */
struct repair_state repair[3];

static int repair_route(struct context *c, const struct packet *p) {
    struct repair_state *s=&repair[c->id];
    if (s->phase==REPAIR_ISOLATED && ((c->id==0)!=(p->to==0))) {
        s->dropped++; s->last_dropped=*p; return 0;
    }
    /* The initial no-op and its genuine ACK cross before the partition. */
    if(c->id!=0 && s->phase==REPAIR_OPEN && p->kind==APPEND_REPLY && p->to==0 &&
       raft_get_current_idx(c->server)==1) s->phase=REPAIR_ISOLATED;
    if(c->id==0 && s->phase==REPAIR_HEALED && p->kind==APPEND_REPLY && p->to==1) {
        s->replies++; s->rejected+=!p->appended.success; s->last_reply=*p;
    }
    return 1;
}
static void repair_receive(struct context *c, const struct packet *p) {
    struct repair_state *s=&repair[c->id];
    if(c->id==0 && p->kind==APPEND && p->from==1 && p->append.term==2 && p->append.leader_commit==3) {
        s->phase=REPAIR_HEALED; s->healed_requests++; s->requests++; s->last_request=*p;
    }
    last_received=*p; received_events++;
    receive(c,p);
    if(c->id!=0 && raft_get_commit_idx(c->server)==3) s->phase=REPAIR_HEALED;
}
static void repair_generate(struct context *c) {
    if(!raft_is_leader(c->server) || c->generated) return;
    raft_term_t term=raft_get_current_term(c->server);
    int command;
    if(c->id==0 && term==1 && raft_get_commit_idx(c->server)==1) {
        repair[0].phase=REPAIR_ISOLATED; command=0;
    } else if(c->id==1 && term==2 && raft_get_current_idx(c->server)==2) command=1;
    else return;
    size_t length=strlen(commands[command])+1;
    raft_entry_t *e=raft_entry_new(length); assert(e);
    e->type=RAFT_LOGTYPE_NORMAL; e->id=command+1;
    memcpy(e->data,commands[command],length);
    raft_entry_resp_t response={0};
    assert(!raft_recv_entry(c->server,e,&response)); raft_entry_release(e); c->generated++;
}
#ifdef RAFT_NATIVE_REFERENCE
static struct packet repair_deliver(int from,int to,int kind) {
    for(int i=0;i<queued;i++) if(queue[i].from==from && queue[i].to==to && queue[i].kind==kind) {
        struct packet p=queue[i]; memmove(queue+i,queue+i+1,(queued-i-1)*sizeof(*queue)); queued--;
        repair_receive(&cluster[to],&p); return p;
    }
    fprintf(stderr,"missing callback %d->%d kind%d queued%d\n",from,to,kind,queued);
    assert(!"requested real callback message missing"); abort();
}
static void entry_is(int node,int index,raft_term_t term,int type,const char *bytes) {
    raft_entry_t *e=raft_get_entry_from_idx(cluster[node].server,index); assert(e);
    size_t length=bytes?strlen(bytes)+1:0;
    assert(e->term==term && e->type==type && e->data_len==length);
    assert(!length || !memcmp(e->data,bytes,length)); raft_entry_release(e);
}
static void heartbeat(int node) { assert(!raft_periodic_internal(cluster[node].server,201)); }
int main(int argc,char **argv) {
    assert(argc==2); int mutant=!strcmp(argv[1],"mutant");
    for(int i=0;i<3;i++) { initialize(&cluster[i],i); cluster[i].route=repair_route; }
    assert(!raft_election_start(cluster[0].server,0));
    repair_deliver(0,2,VOTE); repair_deliver(2,0,VOTE_REPLY);
    repair_deliver(0,2,VOTE); repair_deliver(2,0,VOTE_REPLY);
    assert(raft_is_leader(cluster[0].server));
    repair_deliver(0,1,APPEND); repair_deliver(0,2,APPEND);
    repair_deliver(1,0,APPEND_REPLY); repair_generate(&cluster[0]);
    assert(!raft_apply_all(cluster[0].server));
    assert(repair[0].phase==REPAIR_ISOLATED && repair[1].phase==REPAIR_ISOLATED && repair[2].phase==REPAIR_ISOLATED);
    /* Release C's leader lease by an actual pre-election, then elect B via C. */
    assert(!raft_election_start(cluster[2].server,0));
    assert(!raft_election_start(cluster[1].server,0));
    repair_deliver(1,2,VOTE); repair_deliver(2,1,VOTE_REPLY);
    repair_deliver(1,2,VOTE); repair_deliver(2,1,VOTE_REPLY);
    assert(raft_is_leader(cluster[1].server) && raft_get_current_term(cluster[1].server)==2);
    repair_generate(&cluster[1]);
    repair_deliver(1,2,APPEND); repair_deliver(2,1,APPEND_REPLY);
    repair_deliver(1,2,APPEND); repair_deliver(2,1,APPEND_REPLY);
    assert(raft_get_commit_idx(cluster[1].server)==3);
    heartbeat(1); repair_deliver(1,2,APPEND);
    for(int i=0;i<3;i++) {
        assert(raft_get_current_idx(cluster[i].server)==(i?3:2));
        assert(raft_get_commit_idx(cluster[i].server)==(i?3:1));
        assert(raft_get_last_applied_idx(cluster[i].server)==(i?3:1));
        entry_is(i,1,1,RAFT_LOGTYPE_NO_OP,NULL);
        entry_is(i,2,i?2:1,i?RAFT_LOGTYPE_NO_OP:RAFT_LOGTYPE_NORMAL,i?NULL:commands[0]);
        if(i) entry_is(i,3,2,RAFT_LOGTYPE_NORMAL,commands[1]);
    }
    assert(raft_is_leader(cluster[0].server) && raft_get_current_term(cluster[0].server)==1);
    assert(raft_is_leader(cluster[1].server) && raft_get_current_term(cluster[1].server)==2);
    assert(raft_is_follower(cluster[2].server) && raft_get_current_term(cluster[2].server)==2);
    assert(cluster[0].generated==1 && cluster[1].generated==1 && !cluster[2].generated);
    assert(!repair[0].healed_requests);
    puts("NATIVE_PREHEAL {\"A_term\":1,\"A_commit\":1,\"BC_term\":2,\"BC_commit\":3,\"A_healed_requests\":0}");
    struct packet p=repair_deliver(1,0,APPEND);
    assert(p.append.prev_log_idx==3 && p.append.leader_commit==3);
    assert(!repair[0].last_reply.appended.success && repair[0].last_reply.appended.current_idx==2);
    repair_deliver(0,1,APPEND_REPLY);
    p=repair_deliver(1,0,APPEND);
    assert(p.append.prev_log_idx==2 && p.append.prev_log_term==2 && p.append.n_entries==1);
    if(!mutant) {
        assert(!repair[0].last_reply.appended.success && repair[0].last_reply.appended.current_idx==1);
        repair_deliver(0,1,APPEND_REPLY); p=repair_deliver(1,0,APPEND);
        assert(p.append.prev_log_idx==1 && p.append.n_entries==2);
    }
    assert(repair[0].last_reply.appended.success);
    assert(raft_get_commit_idx(cluster[0].server)==3 && raft_get_last_applied_idx(cluster[0].server)==3);
    for(int i=0;i<3;i++) {
        entry_is(i,1,1,RAFT_LOGTYPE_NO_OP,NULL);
        entry_is(i,2,(i || !mutant)?2:1,(i || !mutant)?RAFT_LOGTYPE_NO_OP:RAFT_LOGTYPE_NORMAL,(i || !mutant)?NULL:commands[0]);
        entry_is(i,3,2,RAFT_LOGTYPE_NORMAL,commands[1]);
    }
    printf("NATIVE_REPAIR {\"requests\":%lu,\"replies\":%lu,\"rejected\":%lu,\"conflicting_committed_index\":%d}\n",repair[0].requests,repair[0].replies,repair[0].rejected,mutant?2:0);
    puts(mutant?"NATIVE_EXPECTED_VIOLATION":"NATIVE_OK"); return 0;
}
#else
int main(int argc,char **argv) {
    assert(argc==2); self=atoi(argv[1]); assert(self>=0 && self<3);
    struct context c={0}; c.fd=socket(AF_INET,SOCK_DGRAM,0); assert(c.fd>=0);
    struct sockaddr_in a=address(self); assert(!bind(c.fd,(struct sockaddr*)&a,sizeof(a)));
    initialize(&c,self); c.route=repair_route; r=c.server; raft=r;
    struct timeval previous={0};
    for(;;) {
        struct timeval now={0}; assert(!gettimeofday(&now,NULL));
        int elapsed=previous.tv_sec ? (int)((now.tv_sec-previous.tv_sec)*1000+(now.tv_usec-previous.tv_usec)/1000) : 0;
        previous=now; assert(!raft_periodic_internal(r,elapsed)); repair_generate(&c);
        struct packet p={0}; ssize_t n=recvfrom(c.fd,&p,sizeof(p),MSG_DONTWAIT,NULL,NULL);
        if(n>=0) { assert(n==sizeof(p)); repair_receive(&c,&p); }
        else assert(errno==EAGAIN || errno==EWOULDBLOCK);
    }
}
#endif
