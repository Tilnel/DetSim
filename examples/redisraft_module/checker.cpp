/* Actual-module oracle: public saved stdout/file bytes only; no VM/module DWARF. */
#include "api/plugin.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/syscall.h>

namespace {
using Json=nlohmann::json;
constexpr size_t max_file=1024*1024, max_output=256*1024, max_records=10000, max_monitor=2*1024*1024;
struct Missing:std::runtime_error { using std::runtime_error::runtime_error; };
struct Violation:std::runtime_error { using std::runtime_error::runtime_error; };
struct Partial {};
void need(bool condition,const char *message) { if(!condition) throw Missing(message); }
void safety(bool condition,const std::string &message) { if(!condition) throw Violation(message); }
uint64_t unsigned_number(std::string_view text) {
    uint64_t value=0; auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
    need(error==std::errc{} && end==text.data()+text.size() && text==std::to_string(value),"invalid unsigned log integer");
    return value;
}
int64_t signed_number(std::string_view text) {
    int64_t value=0; auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
    need(error==std::errc{} && end==text.data()+text.size() && text==std::to_string(value),"invalid signed log integer");
    return value;
}
uint64_t json_number(const Json &value) {
    need(value.is_number_integer() && (!value.is_number_unsigned() ? value.get<int64_t>()>=0 : true),"invalid observation integer");
    auto result=value.get<uint64_t>(); need(result<=INT64_MAX,"observation integer exceeds model limit"); return result;
}
uint32_t crc32c(uint32_t seed,std::string_view bytes) {
    uint32_t value=~seed;
    for(unsigned char byte:bytes) {
        value^=byte;
        for(unsigned bit=0;bit<8;bit++) value=(value>>1)^((value&1)?0x82f63b78u:0);
    }
    return ~value;
}
std::string hex(std::string_view bytes) {
    constexpr char digits[]="0123456789abcdef";
    std::string result; result.reserve(bytes.size()*2);
    for(unsigned char byte:bytes) { result+=digits[byte>>4]; result+=digits[byte&15]; }
    return result;
}
std::string unhex(const std::string &text) {
    need(text.size()%2==0 && text.size()<=2*max_file,"invalid monitor entry bytes");
    auto digit=[](char c)->unsigned { if(c>='0' && c<='9')return c-'0'; if(c>='a' && c<='f')return c-'a'+10;
        throw Missing("noncanonical monitor entry bytes"); };
    std::string result;
    for(size_t i=0;i<text.size();i+=2) result+=static_cast<char>((digit(text[i])<<4)|digit(text[i+1]));
    return result;
}
struct Record { std::array<std::string,7> fields; size_t crc_start=0; };
std::string_view line(std::string_view text,size_t &at) {
    auto end=text.find("\r\n",at); if(end==text.npos) throw Partial{};
    auto result=text.substr(at,end-at); at=end+2; return result;
}
Record record(std::string_view text,size_t &at) {
    need(line(text,at)=="*7","invalid Raft log RESP record");
    Record result;
    for(size_t i=0;i<7;i++) {
        if(i==6) result.crc_start=at;
        auto length=line(text,at); need(!length.empty() && length.front()=='$',"Raft log elements must be bulk strings");
        auto size=unsigned_number(length.substr(1)); need(size<=max_file,"Raft log bulk exceeds limit");
        if(size+2>text.size()-at) throw Partial{};
        result.fields[i]=text.substr(at,size); at+=size;
        need(text.substr(at,2)=="\r\n","invalid Raft log bulk terminator"); at+=2;
    }
    return result;
}
struct Entry {
    std::string term,id,session,type,data;
    bool operator==(const Entry &)const=default;
    Json encode()const { return Json::array({term,id,session,type,hex(data)}); }
    static Entry decode(const Json &value) {
        need(value.is_array() && value.size()==5,"invalid monitor entry");
        for(const auto &item:value) need(item.is_string(),"invalid monitor entry field");
        Entry e{value[0],value[1],value[2],value[3],unhex(value[4])};
        unsigned_number(e.term); signed_number(e.id); unsigned_number(e.session);
        need(unsigned_number(e.type)<=INT32_MAX,"invalid entry type"); return e;
    }
};
struct Log { bool ready=false,partial=false; std::string dbid; uint64_t node=0,base=0; std::map<uint64_t,Entry> entries; };
Log parse_log(const std::string &bytes) {
    need(bytes.size()<=max_file,"Raft log exceeds bounded case limit");
    Log log; size_t at=0; uint32_t crc=0;
    try {
        auto header=record(bytes,at);
        need(header.fields[0]=="RAFTLOG" && header.fields[1]=="1","unsupported Raft log header");
        log.dbid=header.fields[2]; need(!log.dbid.empty() && log.dbid.size()<=64,"invalid Raft dbid");
        log.node=unsigned_number(header.fields[3]);
        unsigned_number(header.fields[4]); log.base=unsigned_number(header.fields[5]);
        need(log.base==0,"compacted log prefix unsupported by this bounded profile");
        auto expected=unsigned_number(header.fields[6]); need(expected<=UINT32_MAX,"invalid header CRC");
        crc=crc32c(0,std::string_view(bytes).substr(0,header.crc_start));
        need(crc==expected,"Raft log header CRC mismatch"); log.ready=true;
        while(at<bytes.size()) {
            auto start=at; auto item=record(bytes,at);
            need(item.fields[0]=="ENTRY","invalid Raft log entry tag");
            Entry entry{item.fields[1],item.fields[2],item.fields[3],item.fields[4],item.fields[5]};
            unsigned_number(entry.term); signed_number(entry.id); unsigned_number(entry.session);
            need(unsigned_number(entry.type)<=INT32_MAX,"invalid log entry type");
            auto expected_entry=unsigned_number(item.fields[6]); need(expected_entry<=UINT32_MAX,"invalid entry CRC");
            crc=crc32c(crc,std::string_view(bytes).substr(start,item.crc_start-start));
            need(crc==expected_entry,"Raft log entry CRC mismatch");
            need(log.entries.size()<max_records,"Raft log entry count exceeds limit");
            log.entries.emplace(log.base+log.entries.size()+1,std::move(entry));
        }
    } catch(const Partial &) { log.partial=true; }
    return log;
}
Json fresh() {
    return {{"v",1},{"stdout",""},{"initialized",false},{"joined",Json::array()},{"members",false},{"acks",Json::object()},
        {"progress",Json::object()},{"locals",Json::object()},{"committed",Json::object()},
        {"done",false},{"bad",""}};
}
void consume(Json &state,const std::string &output);
Json decode_monitor(detsim_bytes bytes) {
    need(bytes.data && bytes.size && bytes.size<=max_monitor,"missing/oversized module monitor");
    const std::string text(reinterpret_cast<const char *>(bytes.data),bytes.size);
    auto state=Json::parse(text);
    need(state.is_object() && state.size()==fresh().size() && state.at("v")==1 && state.dump()==text,"invalid/noncanonical module monitor");
    need(state.at("stdout").is_string() && state.at("stdout").get_ref<const std::string&>().size()<=max_output,"invalid monitor stdout");
    need(state.at("initialized").is_boolean() && state.at("joined").is_array() && state["joined"].size()<=2,"invalid monitor initialization");
    for(auto &node:state["joined"]) need(json_number(node)==2 || json_number(node)==3,"invalid monitor join");
    need(state.at("members").is_boolean() && state.at("done").is_boolean() && state.at("bad").is_string(),"invalid monitor flags");
    for(const char *field:{"acks","progress","locals","committed"}) need(state.at(field).is_object(),"invalid monitor map");
    need(state["acks"].size()<=2 && state["progress"].size()<=3 && state["locals"].size()<=6 && state["committed"].size()<=max_records,"monitor map exceeds limit");
    const auto &consumed=state["stdout"].get_ref<const std::string&>();
    need(consumed.empty() || consumed.back()=='\n',"monitor stdout ends inside a record");
    for(auto &ack:state["acks"].items()) {
        need(ack.key()=="alpha" || ack.key()=="beta","invalid monitor ACK key");
        need(json_number(ack.value())>0 && json_number(ack.value())<=max_records,"invalid monitor ACK index");
    }
    for(auto &progress:state["progress"].items()) {
        need(progress.key()=="1" || progress.key()=="2" || progress.key()=="3","invalid monitor node");
        need(progress.value().is_object() && progress.value().size()==3,"invalid monitor progress");
        for(const char *field:{"term","commit","applied"}) json_number(progress.value().at(field));
        need(json_number(progress.value()["applied"])<=json_number(progress.value()["commit"]),"invalid monitor applied/commit");
    }
    for(auto &local:state["locals"].items()) {
        need(local.key()=="1:alpha" || local.key()=="1:beta" || local.key()=="2:alpha" || local.key()=="2:beta" ||
             local.key()=="3:alpha" || local.key()=="3:beta","invalid monitor local key");
        need(local.value().is_boolean(),"invalid monitor local value");
    }
    for(size_t index=1;index<=state["committed"].size();++index)
        need(state["committed"].contains(std::to_string(index)),"monitor committed prefix has a gap");
    for(auto it=state["committed"].begin();it!=state["committed"].end();++it) {
        need(unsigned_number(it.key())>0,"invalid committed index"); Entry::decode(it.value()); }
    auto replay=fresh();
    try { consume(replay,state["stdout"]); }
    catch(const std::exception &) { throw Missing("invalid monitor observation history"); }
    for(const char *field:{"stdout","initialized","joined","members","acks","progress","locals","done","bad"})
        need(replay[field]==state[field],"monitor fields differ from recorded observations");
    return state;
}
void consume(Json &state,const std::string &output) {
    need(output.size()<=max_output,"client observations exceed limit");
    auto consumed=state["stdout"].get<std::string>();
    safety(output.starts_with(consumed),"acknowledged client observation history rolled back");
    size_t at=consumed.size();
    while(at<output.size()) {
        auto end=output.find('\n',at); if(end==output.npos) break;
        auto item=Json::parse(output.substr(at,end-at)); need(item.is_object() && item.at("v")==1,"invalid client observation schema");
        const std::string event=item.at("event");
        if(event=="initialized") { need(json_number(item.at("node"))==1,"unexpected initialized node"); state["initialized"]=true; }
        else if(event=="joined") { auto node=json_number(item.at("node")); need(node==2 || node==3,"unexpected joined node");
            safety(state["initialized"].get<bool>() && std::find(state["joined"].begin(),state["joined"].end(),node)==state["joined"].end(),"invalid repeated/out-of-order join"); state["joined"].push_back(node); }
        else if(event=="members_ready") { need(json_number(item.at("nodes"))==3,"membership observation incomplete");
            safety(state["initialized"].get<bool>() && state["joined"].size()==2,"membership readiness lacks INIT/JOIN acknowledgements"); state["members"]=true; }
        else if(event=="ack") {
            const std::string key=item.at("key"), value=item.at("value");
            need((key=="alpha" && value=="17") || (key=="beta" && value=="29"),"unexpected acknowledged command");
            safety(state["members"].get<bool>(),"SET acknowledged before membership readiness");
            auto index=json_number(item.at("index")); need(index>0 && index<=max_records,"invalid acknowledged index");
            safety(!state["acks"].contains(key) || state["acks"][key]==index,"acknowledged index changed");
            state["acks"][key]=index;
            if(key=="beta") safety(state["acks"].contains("alpha") && index>json_number(state["acks"]["alpha"]),"two distinct commands did not advance commit");
        } else if(event=="progress") {
            auto node=json_number(item.at("node")); need(node>=1 && node<=3,"invalid observed node");
            auto term=json_number(item.at("term")),commit=json_number(item.at("commit")),applied=json_number(item.at("applied"));
            safety(applied<=commit,"applied exceeds commit"); const auto key=std::to_string(node);
            if(state["progress"].contains(key)) for(const char *field:{"term","commit","applied"})
                safety(json_number(item.at(field))>=json_number(state["progress"][key][field]),"observed term/commit/applied rolled back");
            state["progress"][key]={{"term",term},{"commit",commit},{"applied",applied}};
        } else if(event=="local") {
            auto node=json_number(item.at("node")); const std::string key=item.at("key");
            need(node>=1 && node<=3 && (key=="alpha" || key=="beta"),"invalid local observation");
            need(item.at("present").is_boolean() && item.at("matches").is_boolean(),"invalid local-value flags");
            safety(state["acks"].contains(key) && state["progress"].contains(std::to_string(node)) &&
                json_number(state["progress"][std::to_string(node)]["applied"])>=json_number(state["acks"][key]),"local value checked before acknowledged apply");
            if(!item["present"].get<bool>() || !item["matches"].get<bool>())
                state["bad"]="acknowledged local value missing at node "+std::to_string(node)+" key "+key;
            state["locals"][std::to_string(node)+":"+key]=item["matches"];
        } else if(event=="done") { safety(state["acks"].size()==2 && state["locals"].size()==6,"client completion lacks business evidence"); state["done"]=true; }
        else if(event=="violation") { need(item.at("property")=="applied_local_value","unexpected client violation"); safety(!state["bad"].get<std::string>().empty(),"client violation lacks local-value evidence"); }
        else need(event=="connected" || event=="before_set","unknown client observation event");
        at=end+1;
    }
    state["stdout"]=output.substr(0,at);
}
// serialization.c uses LF-terminated metadata and length-delimited command argv.
// Consume the entire container so ACL/bulk payloads cannot stand in for commands.
bool exact_set(std::string_view bytes,std::string_view key,std::string_view value) {
    need(bytes.size()<=max_file,"command container exceeds bounded case limit");
    struct Reader {
        std::string_view bytes;
        size_t at=0;
        uint64_t number(char prefix) {
            need(at<bytes.size() && bytes[at++]==prefix,"invalid command container field");
            const auto end=bytes.find('\n',at);
            need(end!=bytes.npos && end-at<=20,"missing/oversized command container integer");
            auto result=unsigned_number(bytes.substr(at,end-at));at=end+1;return result;
        }
        std::string_view bulk() {
            const auto size=number('$');
            need(size<=max_file && size<bytes.size()-at,"invalid command container bulk length");
            const auto result=bytes.substr(at,size);at+=size;
            need(bytes[at++]=='\n',"invalid command container bulk terminator");return result;
        }
    } reader{bytes};
    need(reader.number('*')==2 && reader.number('*')==3,"invalid command container metadata wrapper");
    need(reader.number(':')<=1,"invalid command container asking flag");
    reader.number(':');
    reader.bulk(); // ACL is metadata, never a command.
    const auto commands=reader.number('*');
    need(commands>0 && commands<=max_records,"invalid command container count");
    bool matches=commands==1;
    const std::array<std::string_view,3> expected{"SET",key,value};
    for(uint64_t command=0;command<commands;command++) {
        const auto argc=reader.number('*');
        need(argc>0 && argc<=max_records,"invalid command argument count");
        matches=matches && argc==expected.size();
        for(uint64_t arg=0;arg<argc;arg++) {
            const auto token=reader.bulk();
            if(arg>=expected.size() || token!=expected[arg])matches=false;
        }
    }
    need(reader.at==bytes.size(),"trailing bytes after command container");
    return matches;
}
void prefixes(Json &state,const std::array<Log,3> &logs) {
    std::string dbid;
    for(unsigned node=0;node<3;node++) {
        uint64_t required=0; auto key=std::to_string(node+1);
        if(state["progress"].contains(key)) required=json_number(state["progress"][key]["commit"]);
        if(node==0) for(auto &index:state["acks"].items()) required=std::max(required,json_number(index.value()));
        need(required<=max_records,"committed prefix exceeds case bound");
        const auto &log=logs[node];
        if(!log.ready) { need(!state["members"].get<bool>() && required==0,"required saved Raft log header unavailable"); continue; }
        need(log.node==node+1,"saved Raft log node ID differs from configuration");
        if(dbid.empty()) dbid=log.dbid; else safety(dbid==log.dbid,"cluster log dbids differ");
        for(uint64_t index=1;index<=required;index++) {
            auto found=log.entries.find(index); need(found!=log.entries.end(),"confirmed committed prefix has missing/partial entry");
            const auto position=std::to_string(index); auto encoded=found->second.encode();
            safety(!state["committed"].contains(position) || state["committed"][position]==encoded,"conflicting committed command bytes at index "+position);
            state["committed"][position]=std::move(encoded);
        }
    }
    for(auto &ack:state["acks"].items()) {
        const auto position=std::to_string(json_number(ack.value()));
        need(state["committed"].contains(position),"acknowledged command missing from committed history");
        auto entry=Entry::decode(state["committed"][position]);
        safety(entry.type=="0" && exact_set(entry.data,ack.key(),ack.key()=="alpha"?"17":"29"),
            "acknowledged index is not the exact intended NORMAL command");
    }
    safety(state["bad"].get<std::string>().empty(),state["bad"].get<std::string>());
}
int append_string(void *context,const void *bytes,size_t size) {
    auto &text=*static_cast<std::string*>(context);
    if(size>max_file*4-text.size()) return -1;
    try { text.append(static_cast<const char*>(bytes),size); return 0; } catch(...) { return -1; }
}
std::string read_file(const detsim_host_v2 *host,const detsim_state_view *view,unsigned process,int fd,size_t limit,
                      detsim_buffer_kind kind=DETSIM_BUFFER_FILE) {
    need(host->read_buffer,"saved file reader unavailable"); std::string data(limit+1,'\0');size_t copied=0;
    const detsim_buffer_selector selected{kind,fd,0,0,0};
    need(!host->read_buffer(view,process,&selected,data.data(),data.size(),&copied),"saved file bytes unavailable");
    need(copied<=limit,"saved file exceeds case bound");data.resize(copied);return data;
}
void inspect(const detsim_host_v2 *host,const detsim_state_view *view,Json &state) {
    need(host->process_count && host->process_count(view)==4 && host->inspect_json,"full module case requires NP4 saved views");
    need(host->configuration,"module checker configuration unavailable"); auto config=Json::parse(host->configuration);
    need(config.at("logs").is_array() && config["logs"].size()==3,"three explicit log paths required");
    consume(state,read_file(host,view,3,1,max_output,DETSIM_BUFFER_STDIO)); std::array<Log,3> logs;
    for(unsigned node=0;node<3;node++) {
        std::string metadata; need(!host->inspect_json(view,node,{&metadata,append_string}),"saved open-file metadata unavailable");
        auto files=Json::parse(metadata).at("open_files");need(files.is_array(),"invalid saved open-file metadata");
        std::optional<int> descriptor;
        for(const auto &file:files) if(file.at("path")==config["logs"][node]) {
            int fd=file.at("fd");need(!descriptor,"ambiguous Raft log descriptor");descriptor=fd; }
        if(descriptor) logs[node]=parse_log(read_file(host,view,node,*descriptor,max_file));
    }
    prefixes(state,logs);
}
template<class F> detsim_verdict guarded(detsim_diagnostic diagnostic,F action) {
    try { action(); return DETSIM_PASS; }
    catch(const Violation &error) { if(diagnostic.capacity)std::snprintf(diagnostic.data,diagnostic.capacity,"%s",error.what()); return DETSIM_VIOLATION; }
    catch(const std::exception &error) { if(diagnostic.capacity)std::snprintf(diagnostic.data,diagnostic.capacity,"%s",error.what()); return DETSIM_UNAVAILABLE; }
    catch(...) { return DETSIM_ERROR; }
}
void write_monitor(const Json &state,detsim_writer out) {
    const auto bytes=state.dump();need(bytes.size()<=max_monitor,"module monitor exceeds bound");
    need(out.append && !out.append(out.context,bytes.data(),bytes.size()),"cannot emit module monitor");
}
detsim_verdict init(const detsim_host_v2 *host,const detsim_state_view *view,const char*,detsim_writer out,detsim_diagnostic diagnostic) {
    return guarded(diagnostic,[&]{auto state=fresh();inspect(host,view,state);write_monitor(state,out);});
}
detsim_verdict advance(const detsim_host_v2 *host,const detsim_state_view*,const detsim_state_view *view,const detsim_event_v2 *event,
    detsim_bytes previous,detsim_writer out,detsim_diagnostic diagnostic) {
    return guarded(diagnostic,[&]{auto state=decode_monitor(previous);inspect(host,view,state);
        if(event && event->phase==DETSIM_EXECUTION_STOPPED) throw Violation("full module process crashed");
        if(event && event->process==3 && event->phase==DETSIM_THREAD_EXITED && event->syscall_number==SYS_exit_group)
            safety(event->args[0]==0 && state["done"].get<bool>(),"client exited without checked business completion");
        write_monitor(state,out);});
}
const detsim_plugin_v2 plugin{DETSIM_PLUGIN_ABI_V2,sizeof(detsim_plugin_v2),"cases.redisraft-full",1,nullptr,nullptr,init,advance,nullptr,nullptr};
}
extern "C" const detsim_plugin_v2 *detsim_plugin_entry_v2() { return &plugin; }
