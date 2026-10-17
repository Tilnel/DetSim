/* Bounded three-node real-library case. No Raft compaction; two commands only. */
#include "raft.h"
#include "raft_private.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

enum { VOTE=1, VOTE_REPLY, APPEND, APPEND_REPLY, MAX_ENTRIES=2, PAYLOAD=32 };
struct packet {
    int kind, from, to;
    msg_requestvote_t vote;
    msg_requestvote_response_t reply;
    msg_appendentries_t append;
    msg_appendentries_response_t appended;
    raft_entry_t entries[MAX_ENTRIES];
    char payload[MAX_ENTRIES][PAYLOAD];
};
struct context { int id, fd, generated; raft_server_t *server; };
/* Public DWARF root used by the case checker, not a mirror of library state. */
raft_server_t *r;
raft_server_t *raft;
int self;
struct packet last_received;
struct packet last_sent;
unsigned long sent_events;
unsigned long received_events;
static const char commands[2][PAYLOAD] = { "set alpha 17", "set beta 29" };
#ifdef RAFT_NATIVE_REFERENCE
static struct context cluster[3];
static struct packet queue[4096];
static int queued;
#endif
#ifndef RAFT_NATIVE_REFERENCE
static struct sockaddr_in address(int id) {
    struct sockaddr_in a = {0};
    a.sin_family=AF_INET; a.sin_port=htons(9000);
    a.sin_addr.s_addr=htonl(0xc0a80001u+(unsigned)id);
    return a;
}
#endif
static int emit(struct context *c, struct packet *p) {
    p->from=c->id;
    last_sent=*p;
    sent_events++;
#ifdef RAFT_NATIVE_REFERENCE
    assert(queued < (int)(sizeof(queue)/sizeof(*queue)));
    queue[queued++]=*p;
#else
    struct sockaddr_in a=address(p->to);
    assert(sendto(c->fd,p,sizeof(*p),0,(struct sockaddr*)&a,sizeof(a)) == sizeof(*p));
#endif
    return 0;
}
static int vote(raft_server_t *s, void *u, raft_node_t *n, msg_requestvote_t *m) {
    (void)s; struct packet p={0}; p.kind=VOTE; p.to=raft_node_get_id(n); p.vote=*m;
    return emit(u,&p);
}
static int append(raft_server_t *s, void *u, raft_node_t *n, msg_appendentries_t *m) {
    (void)s; struct packet p={0}; p.kind=APPEND; p.to=raft_node_get_id(n); p.append=*m;
    p.append.entries=NULL; assert(m->n_entries>=0 && m->n_entries<=MAX_ENTRIES);
    for (int i=0;i<m->n_entries;i++) {
        p.entries[i]=m->entries[i]; p.entries[i].data.buf=NULL;
        assert(m->entries[i].data.len<=PAYLOAD);
        memcpy(p.payload[i],m->entries[i].data.buf,m->entries[i].data.len);
    }
    return emit(u,&p);
}
static int apply(raft_server_t *s, void *u, raft_entry_t *e, raft_index_t i) {
    (void)s; (void)u; assert(i>=1 && i<=2); assert(e->type==RAFT_LOGTYPE_NORMAL);
    assert(e->data.len==strlen(commands[i-1])+1);
    assert(!memcmp(e->data.buf,commands[i-1],e->data.len)); return 0;
}
static int offer(raft_server_t *s, void *u, raft_entry_t *e, raft_index_t index) {
    (void)s; (void)u; (void)index;
    void *copy=malloc(e->data.len); assert(copy);
    memcpy(copy,e->data.buf,e->data.len); e->data.buf=copy; return 0;
}
static int release(raft_server_t *s, void *u, raft_entry_t *e, raft_index_t index) {
    (void)s; (void)u; (void)index;
    free(e->data.buf); e->data.buf=NULL; return 0;
}
static void initialize(struct context *c, int id) {
    c->id=id; c->server=raft_new(); assert(c->server);
    raft_cbs_t cb={0}; cb.send_requestvote=vote; cb.send_appendentries=append; cb.applylog=apply;
    cb.log_offer=offer; cb.log_pop=release;
    raft_set_callbacks(c->server,&cb,c);
    raft_set_election_timeout(c->server,1000);
    for(int i=0;i<3;i++) assert(raft_add_node(c->server,NULL,i,i==id));
}
static void receive(struct context *c, const struct packet *wire) {
    struct packet p=*wire; assert(p.to==c->id && p.from>=0 && p.from<3);
    raft_node_t *sender=raft_get_node(c->server,p.from);
    struct packet response={0}; response.to=p.from;
    switch(p.kind) {
    case VOTE:
        response.kind=VOTE_REPLY;
        assert(!raft_recv_requestvote(c->server,sender,&p.vote,&response.reply));
        emit(c,&response); break;
    case VOTE_REPLY:
        assert(!raft_recv_requestvote_response(c->server,sender,&p.reply)); break;
    case APPEND:
        assert(p.append.n_entries>=0 && p.append.n_entries<=MAX_ENTRIES);
        p.append.entries=p.entries;
        for(int i=0;i<p.append.n_entries;i++) {
            assert(p.entries[i].data.len<=PAYLOAD);
            /* log_offer deep-copies into each retained log entry. */
            p.entries[i].data.buf=p.payload[i];
        }
        response.kind=APPEND_REPLY;
        assert(!raft_recv_appendentries(c->server,sender,&p.append,&response.appended));
        emit(c,&response); break;
    case APPEND_REPLY:
        assert(!raft_recv_appendentries_response(c->server,sender,&p.appended)); break;
    default: assert(0);
    }
    assert(!raft_apply_all(c->server));
}
static void generate(struct context *c) {
    if(!raft_is_leader(c->server) || c->generated==2) return;
    raft_entry_t e={0}; e.type=RAFT_LOGTYPE_NORMAL; e.id=c->generated+1;
    e.data.buf=(void*)commands[c->generated]; e.data.len=strlen(e.data.buf)+1;
    msg_entry_response_t response={0};
    assert(!raft_recv_entry(c->server,&e,&response)); c->generated++;
}
#ifdef RAFT_NATIVE_REFERENCE
static struct packet deliver(int from,int to,int kind) {
    for(int i=0;i<queued;i++) if(queue[i].from==from && queue[i].to==to && queue[i].kind==kind) {
        struct packet p=queue[i]; memmove(queue+i,queue+i+1,(queued-i-1)*sizeof(*queue)); queued--;
        receive(&cluster[to],&p); return p;
    }
    assert(!"requested real callback message missing"); abort();
}
int main(int argc,char **argv) {
    assert(argc==2); int mutant=!strcmp(argv[1],"mutant");
    for(int i=0;i<3;i++) initialize(&cluster[i],i);
    assert(!raft_become_candidate(cluster[0].server));
    assert(!raft_become_candidate(cluster[1].server));
    struct packet grant=deliver(0,2,VOTE), deny=deliver(1,2,VOTE);
    (void)grant; (void)deny;
    assert(raft_get_voted_for(cluster[2].server)==0);
    struct packet yes=deliver(2,0,VOTE_REPLY);
    assert(yes.reply.vote_granted==RAFT_REQUESTVOTE_ERR_GRANTED);
    assert(raft_is_leader(cluster[0].server));
    struct packet no=deliver(2,1,VOTE_REPLY);
    assert(no.reply.vote_granted==RAFT_REQUESTVOTE_ERR_NOT_GRANTED);
    assert(raft_get_current_term(cluster[0].server)==1 && raft_get_current_term(cluster[1].server)==1);
    int dual=raft_is_leader(cluster[1].server);
    printf("NATIVE_ELECTION {\"term\":1,\"C_vote\":0,\"C_grant_A\":1,\"C_deny_B\":0,\"leaders\":%d}\n",1+dual);
    assert(dual==mutant);
    if(!mutant) {
        /* Restore normal delivery after the controlled election prefix. */
        for(int rounds=0;rounds<100;rounds++) {
            generate(&cluster[0]);
            if(queued) { struct packet p=queue[0]; memmove(queue,queue+1,--queued*sizeof(*queue)); receive(&cluster[p.to],&p); }
            for(int i=0;i<3;i++) assert(!raft_periodic(cluster[i].server,0));
            if(!queued) assert(!raft_periodic(cluster[0].server,201));
            if(raft_get_commit_idx(cluster[0].server)==2 && raft_get_commit_idx(cluster[1].server)==2 && raft_get_commit_idx(cluster[2].server)==2) break;
        }
        for(int i=0;i<3;i++) { assert(raft_get_commit_idx(cluster[i].server)==2); assert(!raft_apply_all(cluster[i].server)); }
        puts("NATIVE_COMMIT {\"commands\":[\"set alpha 17\",\"set beta 29\"],\"all_commit_idx\":2}");
    }
    puts(mutant?"NATIVE_EXPECTED_VIOLATION":"NATIVE_OK"); return 0;
}
#else
int main(int argc,char **argv) {
    assert(argc==2); self=atoi(argv[1]); assert(self>=0 && self<3);
    struct context c={0}; c.fd=socket(AF_INET,SOCK_DGRAM,0); assert(c.fd>=0);
    struct sockaddr_in a=address(self); assert(!bind(c.fd,(struct sockaddr*)&a,sizeof(a)));
    initialize(&c,self); r=c.server; raft=r;
    struct timeval previous={0};
    for(;;) {
        struct timeval now={0}; assert(!gettimeofday(&now,NULL));
        int elapsed=previous.tv_sec ? (int)((now.tv_sec-previous.tv_sec)*1000+(now.tv_usec-previous.tv_usec)/1000) : 0;
        previous=now; assert(!raft_periodic(r,elapsed)); generate(&c);
        struct packet p={0}; ssize_t n=recvfrom(c.fd,&p,sizeof(p),MSG_DONTWAIT,NULL,NULL);
        if(n>=0) { assert(n==sizeof(p)); last_received=p; received_events++; receive(&c,&p); }
        else assert(errno==EAGAIN || errno==EWOULDBLOCK);
    }
}
#endif
