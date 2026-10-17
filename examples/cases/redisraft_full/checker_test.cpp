#include "checker.cpp"
#include <cstring>
#include <fstream>
#include <vector>
#include <iostream>
struct detsim_state_view {
    std::string output;
    std::array<std::string,3> logs;
    bool readable=true;
    bool output_open=true;
};
namespace {
size_t count(const detsim_state_view*) { return 4; }
int metadata(const detsim_state_view *view,uint32_t process,detsim_writer writer) {
    if(!view->readable || process>=3)return -1;
    auto text=Json{{"open_files",Json::array({{{"fd",100+process},{"path","/logs/"+std::to_string(process+1)}}})}}.dump();
    return writer.append(writer.context,text.data(),text.size());
}
int saved(const detsim_state_view *view,uint32_t process,const detsim_buffer_selector *selection,void *out,size_t size,size_t *copied) {
    if(!view->readable || selection->offset || selection->item || selection->message)return -1;
    const std::string *text=nullptr;
    if(process==3 && selection->fd==1 &&
       (selection->kind==DETSIM_BUFFER_STDIO || (selection->kind==DETSIM_BUFFER_FILE && view->output_open)))text=&view->output;
    else if(selection->kind==DETSIM_BUFFER_FILE && process<3 && selection->fd==100+static_cast<int>(process))text=&view->logs[process];
    if(!text)return -1;
    *copied=std::min(size,text->size()); std::memcpy(out,text->data(),*copied);return 0;
}
const detsim_host_v2 host{DETSIM_PLUGIN_ABI_V2,count,nullptr,nullptr,nullptr,
    "{\"logs\":[\"/logs/1\",\"/logs/2\",\"/logs/3\"]}",metadata,saved};
struct Buffer {
    std::string bytes;
    static int append(void *context,const void *data,size_t size) {static_cast<Buffer*>(context)->bytes.append(static_cast<const char*>(data),size);return 0;}
    detsim_writer writer() {return {this,append};}
    detsim_bytes view()const {return {reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size()};}
};
void expect(bool okay,const char *message) { if(!okay)throw std::runtime_error(message); }
std::string bulk(const std::string &text) {return "$"+std::to_string(text.size())+"\r\n"+text+"\r\n";}
std::string encoded(const std::array<std::string,6> &fields,uint32_t &crc) {
    std::string body="*7\r\n"; for(const auto &field:fields)body+=bulk(field);
    crc=crc32c(crc,body);return body+bulk(std::to_string(crc));
}
std::string logfile(unsigned node,const std::vector<Entry> &entries) {
    uint32_t crc=0; auto bytes=encoded({"RAFTLOG","1","same-cluster",std::to_string(node),"0","0"},crc);
    for(const auto &e:entries)bytes+=encoded({"ENTRY",e.term,e.id,e.session,e.type,e.data},crc);
    return bytes;
}
std::string observation(Json item) {item["v"]=1;return item.dump()+"\n";}
std::string output(bool bad=false) {
    std::string text=observation({{"event","initialized"},{"node",1}});
    text+=observation({{"event","joined"},{"node",2}});
    text+=observation({{"event","joined"},{"node",3}});
    text+=observation({{"event","members_ready"},{"nodes",3}});
    text+=observation({{"event","ack"},{"key","alpha"},{"value","17"},{"index",1}});
    text+=observation({{"event","ack"},{"key","beta"},{"value","29"},{"index",2}});
    for(unsigned node=1;node<=3;node++)text+=observation({{"event","progress"},{"node",node},{"term",1},{"commit",2},{"applied",2}});
    for(unsigned node=1;node<=3;node++)for(const char *key:{"alpha","beta"}) {
        bool matches=!(bad && node==3 && std::string_view(key)=="alpha");
        text+=observation({{"event","local"},{"node",node},{"key",key},{"present",matches},{"matches",matches}});
    }
    text+=observation({{"event",bad?"violation":"done"},{"property","applied_local_value"}});return text;
}
std::string load(const char *path) {std::ifstream input(path,std::ios::binary);expect(bool(input),"native file unavailable");return {std::istreambuf_iterator<char>(input),{}};}
}
int main(int argc,char **argv) {
    try {
        char text[512]{};detsim_diagnostic diagnostic{text,sizeof(text)};
        if(argc==6 && std::string_view(argv[1])=="--native") {
            detsim_state_view view{load(argv[2]),{load(argv[3]),load(argv[4]),load(argv[5])}}; Buffer monitor;
            auto result=init(&host,&view,"{}",monitor.writer(),diagnostic);
            std::cout<<"NATIVE_CHECK verdict="<<result<<" diagnostic="<<text<<"\n";
            if(result==DETSIM_PASS) for(unsigned i=0;i<3;i++) {Buffer replay;
                expect(advance(&host,&view,&view,nullptr,monitor.view(),replay.writer(),diagnostic)==DETSIM_PASS && replay.bytes==monitor.bytes,"native monitor roundtrip changed");}
            return result==DETSIM_PASS?0:result==DETSIM_VIOLATION?1:2;
        }
        expect(argc==1,"usage: checker_test [--native stdout log1 log2 log3]");
        expect(crc32c(0,"123456789")==0xe3069283,"CRC32C polynomial/seed mismatch");
        std::vector<Entry> entries{{"1","101","18446744073709551615","0","*3\n$3\nSET\n$5\nalpha\n$2\n17\n"},
            {"1","102","10","0","*3\n$3\nSET\n$4\nbeta\n$2\n29\n"}};
        for(auto &entry:entries) entry.data="*2\n*3\n:0\n:0\n$0\n\n*1\n"+entry.data;
        detsim_state_view good{output(),{logfile(1,entries),logfile(2,entries),logfile(3,entries)}};
        expect(good.logs[0]!=good.logs[1],"node-specific CRC fixture missing");
        Buffer monitor;expect(init(&host,&good,"{}",monitor.writer(),diagnostic)==DETSIM_PASS,"healthy public saved projection failed");
        for(unsigned i=0;i<3;i++) {Buffer next;expect(advance(&host,&good,&good,nullptr,monitor.view(),next.writer(),diagnostic)==DETSIM_PASS && next.bytes==monitor.bytes,"canonical monitor replay failed");}
        auto terminal=good;terminal.output_open=false;
        const detsim_buffer_selector closed_file{DETSIM_BUFFER_FILE,1,0,0,0};
        char byte{};size_t copied=0;
        expect(saved(&terminal,3,&closed_file,&byte,1,&copied)!=0,"closed FILE selector incorrectly readable");
        detsim_event_v2 exited{};exited.process=3;exited.syscall_number=SYS_exit_group;
        exited.phase=DETSIM_THREAD_EXITED;exited.args[0]=0;
        Buffer completed;
        expect(advance(&host,&good,&terminal,&exited,monitor.view(),completed.writer(),diagnostic)==DETSIM_PASS,
               "healthy virtual exit loses saved stdout after fd1 closes");
        auto unavailable_host=host;unavailable_host.read_buffer=nullptr;
        Buffer unavailable;
        expect(advance(&unavailable_host,&good,&terminal,&exited,monitor.view(),unavailable.writer(),diagnostic)==DETSIM_UNAVAILABLE,
               "missing saved stream reader passed at exit");
        auto check=[&](detsim_state_view view,detsim_verdict expected,const char *message) {Buffer ignored;
            expect(init(&host,&view,"{}",ignored.writer(),diagnostic)==expected,message);};
        const std::string set="*3\n$3\nSET\n$5\nalpha\n$2\n17\n";
        auto reject_command=[&](const std::string &data,detsim_verdict verdict,const char *message) {
            auto altered=entries;altered[0].data=data;auto view=good;
            for(unsigned node=0;node<3;node++)view.logs[node]=logfile(node+1,altered);
            check(view,verdict,message);
        };
        const std::string wrapper="*2\n*3\n:0\n:0\n$0\n\n";
        const auto string_arg=[](const std::string &value) {return "$"+std::to_string(value.size())+"\n"+value+"\n";};
        reject_command(wrapper+"*1\n*2\n$4\nECHO\n"+string_arg(set),DETSIM_VIOLATION,"SET text embedded in unrelated bulk argument passed");
        reject_command("*2\n*3\n:0\n:0\n"+string_arg(set)+"*1\n*1\n$4\nPING\n",DETSIM_VIOLATION,"SET text in ACL metadata passed");
        reject_command(entries[0].data+std::string("\0binary",7),DETSIM_UNAVAILABLE,"trailing binary bytes passed");
        reject_command(entries[0].data+"junk",DETSIM_UNAVAILABLE,"trailing text passed");
        reject_command(set,DETSIM_UNAVAILABLE,"unwrapped command passed");
        reject_command(wrapper+"*2\n"+set+"*1\n$4\nPING\n",DETSIM_VIOLATION,"extra command after SET passed");
        reject_command(wrapper+"*2\n*1\n$4\nPING\n"+set,DETSIM_VIOLATION,"SET at second command position passed");
        reject_command(wrapper+"*1\n*3\n$3\nSET\n$4\n17\n$5\nalpha\n",DETSIM_UNAVAILABLE,"malformed bulk length passed");
        for(const char *length:{"-2","18446744073709551616","999999999","03"})
            reject_command(wrapper+"*1\n*3\n$"+length+"\nSET\n$5\nalpha\n$2\n17\n",DETSIM_UNAVAILABLE,"invalid bulk length passed");
        reject_command(wrapper+"*1\n*3\n$3\nSET\n$2\n17\n$5\nalpha\n",DETSIM_VIOLATION,"swapped key/value tokens passed");
        auto bad=good;bad.output=output(true);check(bad,DETSIM_VIOLATION,"applied-but-missing local value passed");
        bad=good;auto changed=entries;changed[0].data.back()='x';bad.logs[2]=logfile(3,changed);
        check(bad,DETSIM_VIOLATION,"same metadata different binary command passed");
        bad=good;bad.logs[2].back()='x';check(bad,DETSIM_UNAVAILABLE,"corrupt CRC terminator passed");
        bad=good;bad.logs[1]=logfile(2,{entries[0]});check(bad,DETSIM_UNAVAILABLE,"missing committed record passed");
        bad=good;bad.logs[2].clear();check(bad,DETSIM_UNAVAILABLE,"unavailable required log passed");
        bad=good;bad.logs[2]+="*7\r\n$5\r\nENT";check(bad,DETSIM_PASS,"uncommitted partial tail rejected");
        bad=good;bad.logs[0]=logfile(2,entries);check(bad,DETSIM_UNAVAILABLE,"wrong node header passed");
        bad=good;bad.readable=false;check(bad,DETSIM_UNAVAILABLE,"missing saved file bytes passed");
        Buffer ignored;expect(advance(&host,&good,&good,nullptr,{nullptr,0},ignored.writer(),diagnostic)==DETSIM_UNAVAILABLE,"missing monitor passed");
        auto truncated=monitor;truncated.bytes.pop_back();
        expect(advance(&host,&good,&good,nullptr,truncated.view(),ignored.writer(),diagnostic)==DETSIM_UNAVAILABLE,"truncated monitor passed");
        auto malformed=Json::parse(monitor.bytes);malformed["acks"]["alpha"]=-1;
        auto malformed_bytes=Buffer{malformed.dump()};
        expect(advance(&host,&good,&good,nullptr,malformed_bytes.view(),ignored.writer(),diagnostic)==DETSIM_UNAVAILABLE,"negative monitor ACK passed");
        malformed=Json::parse(monitor.bytes);malformed["locals"]["3:alpha"]=false;malformed_bytes.bytes=malformed.dump();
        expect(advance(&host,&good,&good,nullptr,malformed_bytes.view(),ignored.writer(),diagnostic)==DETSIM_UNAVAILABLE,"inconsistent monitor local result passed");
        auto wrong_np=host;wrong_np.process_count=[](const detsim_state_view*)->size_t{return 3;};
        expect(init(&wrong_np,&good,"{}",ignored.writer(),diagnostic)==DETSIM_UNAVAILABLE,"NP3 masqueraded as full NP4 case");
        detsim_writer failing{nullptr,[](void*,const void*,size_t){return -1;}};
        expect(init(&host,&good,"{}",failing,diagnostic)==DETSIM_UNAVAILABLE,"failed monitor writer passed");
        auto branch=good;branch.output+=observation({{"event","progress"},{"node",3},{"term",1},{"commit",1},{"applied",1}});
        expect(advance(&host,&good,&branch,nullptr,monitor.view(),ignored.writer(),diagnostic)==DETSIM_VIOLATION,"observed commit rollback passed");
        branch=good;branch.output.clear();
        expect(advance(&host,&good,&branch,nullptr,monitor.view(),ignored.writer(),diagnostic)==DETSIM_VIOLATION,"ACK output rollback passed");
        Buffer independent;expect(advance(&host,&good,&good,nullptr,monitor.view(),independent.writer(),diagnostic)==DETSIM_PASS && independent.bytes==monitor.bytes,"independent branch history leaked");
        auto incomplete=fresh();consume(incomplete,good.output.substr(0,17));expect(incomplete["stdout"]=="","partial observation was consumed");
        for(size_t i=0;i<good.logs[0].size();i++) {auto parsed=parse_log(good.logs[0].substr(0,i));expect(parsed.entries.size()<=2,"partial log manufactured committed entries");}
        std::cout<<"PASS full-module public saved stdout/FD projection, node-local CRC chains, exact binary committed prefix, local oracle, branch monitor, missing/corrupt/partial data\n";
        return 0;
    } catch(const std::exception &error) {std::cerr<<"FAIL "<<error.what()<<"\n";return 2;}
}
