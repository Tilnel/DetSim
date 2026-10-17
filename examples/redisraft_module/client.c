/* Real RESP client. Progress comes from network requests, never sleeps or timer patches. */
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { LIMIT=32768, ATTEMPTS=10000 };
struct connection { int fd; size_t at, end; char buffer[LIMIT]; };
struct reply { char kind; long long number; size_t length; char text[LIMIT]; };
static struct connection connections[3];
static int byte(struct connection *c) {
    if(c->at==c->end) {
        ssize_t n; do { n=read(c->fd,c->buffer,sizeof(c->buffer)); } while(n<0 && errno==EINTR);
        assert(n>0); c->at=0; c->end=(size_t)n;
    }
    return (unsigned char)c->buffer[c->at++];
}
static long long number(struct connection *c) {
    char text[64]; size_t n=0;
    for(;;) { int ch=byte(c); if(ch=='\r') { assert(byte(c)=='\n'); break; }
        assert(n+1<sizeof(text)); text[n++]=(char)ch; }
    text[n]=0; char *end=NULL; errno=0; long long value=strtoll(text,&end,10);
    assert(!errno && n && end==text+n); return value;
}
static struct reply receive(struct connection *c, unsigned depth) {
    assert(depth<8); struct reply r={0}; r.kind=(char)byte(c);
    if(r.kind=='*') {
        r.number=number(c); assert(r.number>=0 && r.number<100);
        for(long long i=0;i<r.number;i++) (void)receive(c,depth+1);
    } else if(r.kind=='$') {
        r.number=number(c); assert(r.number>=-1 && r.number<LIMIT);
        if(r.number>=0) { r.length=(size_t)r.number;
            for(size_t i=0;i<r.length;i++) r.text[i]=(char)byte(c);
            assert(byte(c)=='\r' && byte(c)=='\n'); r.text[r.length]=0; }
    } else if(r.kind==':') r.number=number(c);
    else {
        assert(r.kind=='+' || r.kind=='-');
        for(;;) { int ch=byte(c); if(ch=='\r') { assert(byte(c)=='\n'); break; }
            assert(r.length+1<sizeof(r.text)); r.text[r.length++]=(char)ch; }
        r.text[r.length]=0;
    }
    return r;
}
static struct reply request(int node, int argc, const char **argv) {
    char buffer[LIMIT]; int used=snprintf(buffer,sizeof(buffer),"*%d\r\n",argc); assert(used>0);
    for(int i=0;i<argc;i++) {
        size_t length=strlen(argv[i]);
        int n=snprintf(buffer+used,sizeof(buffer)-(size_t)used,"$%zu\r\n%s\r\n",length,argv[i]);
        assert(n>0 && (size_t)n<sizeof(buffer)-(size_t)used); used+=n;
    }
    size_t sent=0;
    while(sent<(size_t)used) { ssize_t n=write(connections[node].fd,buffer+sent,(size_t)used-sent);
        if(n<0 && errno==EINTR) continue;
        assert(n>0); sent+=(size_t)n; }
    return receive(&connections[node],0);
}
static long long field(const struct reply *r,const char *name) {
    char needle[80]; snprintf(needle,sizeof(needle),"raft_%s:",name);
    const char *p=strstr(r->text,needle); assert(p); p+=strlen(needle);
    char *end=NULL; errno=0; long long value=strtoll(p,&end,10);
    assert(!errno && end!=p && *end=='\r'); return value;
}
static struct reply info(int node) { const char *args[]={"INFO","raft"};
    struct reply r=request(node,2,args); assert(r.kind=='$'); return r; }
static void pump(void) { const char *args[]={"GET","beta"}; struct reply r=request(0,2,args);
    assert(r.kind=='$' || r.kind=='-'); }
static int local(int node,const char *key,const char *expected) {
    const char *args[]={"RAFT.DEBUG","EXEC","GET",key}; struct reply r=request(node,4,args);
    assert(r.kind=='$'); int present=r.number>=0;
    int matches=present && r.length==strlen(expected) && !memcmp(r.text,expected,r.length);
    printf("{\"v\":1,\"event\":\"local\",\"node\":%d,\"key\":\"%s\",\"present\":%s,\"matches\":%s}\n",
           node+1,key,present?"true":"false",matches?"true":"false");
    return matches;
}
int main(int argc,char **argv) {
    assert(argc==7); setvbuf(stdout,NULL,_IOLBF,0);
    for(int node=0;node<3;node++) {
        struct sockaddr_in address={0}; address.sin_family=AF_INET;
        assert(inet_pton(AF_INET,argv[1+node*2],&address.sin_addr)==1);
        int port=atoi(argv[2+node*2]); assert(port>0 && port<=65535); address.sin_port=htons((uint16_t)port);
        connections[node].fd=socket(AF_INET,SOCK_STREAM,0); assert(connections[node].fd>=0);
        assert(!connect(connections[node].fd,(struct sockaddr*)&address,sizeof(address)));
    }
    puts("{\"v\":1,\"event\":\"connected\"}");
    const char *init[]={"RAFT.CLUSTER","INIT"}; struct reply r=request(0,2,init); assert(r.kind=='+' && r.length==35 && !memcmp(r.text,"OK ",3));
    puts("{\"v\":1,\"event\":\"initialized\",\"node\":1}");
    char target[80]; snprintf(target,sizeof(target),"%s:%s",argv[1],argv[2]);
    for(int node=1;node<3;node++) { const char *join[]={"RAFT.CLUSTER","JOIN",target};
        r=request(node,3,join); assert(r.kind=='+' && !strcmp(r.text,"OK"));
        printf("{\"v\":1,\"event\":\"joined\",\"node\":%d}\n",node+1); }
    int ready=0;
    for(int attempt=0;attempt<ATTEMPTS;attempt++) {
        pump(); ready=1;
        for(int node=0;node<3;node++) { r=info(node);
            if(field(&r,"num_nodes")!=3 || field(&r,"num_voting_nodes")!=3) ready=0; }
        if(ready) break;
    }
    assert(ready); puts("{\"v\":1,\"event\":\"members_ready\",\"nodes\":3}");
    const char *keys[]={"alpha","beta"}, *values[]={"17","29"}; long long acknowledged[2]={0};
    for(int key=0;key<2;key++) {
        printf("{\"v\":1,\"event\":\"before_set\",\"key\":\"%s\"}\n",keys[key]);
        const char *set[]={"SET",keys[key],values[key]}; r=request(0,3,set);
        assert(r.kind=='+' && !strcmp(r.text,"OK")); r=info(0);
        acknowledged[key]=field(&r,"commit_index"); assert(acknowledged[key]>0);
        printf("{\"v\":1,\"event\":\"ack\",\"key\":\"%s\",\"value\":\"%s\",\"index\":%lld}\n",
               keys[key],values[key],acknowledged[key]);
    }
    assert(acknowledged[1]>acknowledged[0]); ready=0;
    for(int attempt=0;attempt<ATTEMPTS;attempt++) {
        pump(); ready=1;
        for(int node=0;node<3;node++) { r=info(node);
            long long commit=field(&r,"commit_index"),applied=field(&r,"last_applied_index");
            printf("{\"v\":1,\"event\":\"progress\",\"node\":%d,\"term\":%lld,\"commit\":%lld,\"applied\":%lld}\n",
                   node+1,field(&r,"current_term"),commit,applied);
            if(commit<acknowledged[1] || applied<acknowledged[1]) ready=0; }
        if(ready) break;
    }
    assert(ready); int valid=1;
    for(int node=0;node<3;node++) for(int key=0;key<2;key++) valid &= local(node,keys[key],values[key]);
    puts(valid?"{\"v\":1,\"event\":\"done\"}":"{\"v\":1,\"event\":\"violation\",\"property\":\"applied_local_value\"}");
    for(int node=0;node<3;node++) assert(!close(connections[node].fd));
    return valid?0:42;
}
