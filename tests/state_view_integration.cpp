#include "state/state_view.h"
#include "state/page_snapshot.h"
#include "monitor.h"
#include "proc_status.h"
#include "dwarf_info.h"
#include "cereal/archives/binary.hpp"
#include <sstream>
#include <nlohmann/json.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/deque.hpp>
#include <fcntl.h>
#include "ncurses_ui.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <sys/mman.h>
extern "C" detsim::ui::NCursesUI* get_ncurses_ui() { return nullptr; }
static void require(bool b,const char* m) { if(!b) throw std::runtime_error(m); }
template<class F> static void rejects(F f) { bool b=false; try { f(); } catch(const std::exception&) { b=true; } require(b,"expected rejection"); }
static_assert(NP >= 2, "State observation fixture needs two logical nodes");
// Construct saved observations through both real snapshot layouts, without a live tracee.
static sys_state save_stdio_fixture(const tracee_state& saved, bool pages) {
 auto& store=StateStore::instance();
 std::ostringstream encoded; { cereal::BinaryOutputArchive archive(encoded); archive(saved); }
 const auto text=encoded.str(); const std::vector<uint8_t> metadata(text.begin(),text.end());
 RegionInfo region{}; region.start=0x1000; region.end=0x2000; region.prot=PROT_READ;
 memcpy(region.flags,"r--p",5); const std::vector<uint8_t> memory(4096,0); user_regs_struct regs{};
 std::vector<uint8_t> bytes;
 if (pages) {
  const auto page=store.save(memory.data(),memory.size());
  bytes=encode_page_manifest({region},metadata,"",regs,{{region.start,4096,page}});
 } else {
  StateDataHeader header{}; header.magic=0x4453544D; header.version=FLAT_SNAPSHOT_VERSION; header.num_regions=1;
  header.tstate_offset=sizeof(header)+sizeof(region); header.tstate_size=metadata.size();
  header.maps_offset=header.tstate_offset+header.tstate_size;
  region.offset=header.maps_offset; header.regs_offset=region.offset+memory.size();
  bytes.resize(header.regs_offset+sizeof(regs));
  memcpy(bytes.data(),&header,sizeof(header)); memcpy(bytes.data()+sizeof(header),&region,sizeof(region));
  memcpy(bytes.data()+header.tstate_offset,metadata.data(),metadata.size());
  memcpy(bytes.data()+region.offset,memory.data(),memory.size()); memcpy(bytes.data()+header.regs_offset,&regs,sizeof(regs));
 }
 sys_state machine; machine.ts_hash[0]=store.save(bytes.data(),bytes.size()); return machine;
}
static void captured_stdio_observation(StateStore& store, const StateStore::Config& config) {
 static_assert(DETSIM_BUFFER_STDIO==6 && DETSIM_PLUGIN_ABI_V2==2);
 const auto captured=DETSIM_BUFFER_STDIO;
 auto cold=[&](const sys_state& machine) {
  store.wait_for_completion(); store.shutdown(); store.init(config); return detsim::StateView(machine);
 };
 for (bool pages:{false,true}) {
  tracee_state saved;
  {
   auto empty_view=cold(save_stdio_fixture(saved,pages)); char untouched='?';
   for(int channel=0;channel<3;++channel)
    require(empty_view.read_buffer(0,captured,channel,0,0,0,&untouched,1)==0 && untouched=='?',"existing empty captured stream is valid EOF");
  }
  std::array<std::string,3> keys;
  for(int channel=0;channel<3;++channel) {
   keys[channel]=saved.fs_state.open_files.at(channel).path;
   auto& node=saved.fs_state.filesystem.at(keys[channel]);
   node.content={static_cast<char>('0'+channel),'\0','x'}; node.metadata.st_size=3;
  }
  auto machine=save_stdio_fixture(saved,pages);
  auto view=cold(machine); char result[16];
  for(int channel=0;channel<3;++channel) {
   memset(result,'?',sizeof(result));
   require(view.read_buffer(0,captured,channel,0,0,0,result,sizeof(result))==3 &&
           result[0]=='0'+channel && result[1]=='\0' && result[2]=='x' && result[3]=='?',"captured channel exact binary bytes");
   require(view.read_buffer(0,captured,channel,0,0,1,result,1)==1 && result[0]=='\0',"captured channel offset and bound");
   require(view.read_buffer(0,captured,channel,0,0,3,nullptr,0)==0,"captured channel EOF");
  }
  for(int channel:{-1,3,999}) rejects([&]{view.read_buffer(0,captured,channel,0,0,0,result,1);});
  rejects([&]{view.read_buffer(0,captured,1,1,0,0,result,1);});
  rejects([&]{view.read_buffer(0,captured,1,0,1,0,result,1);});
  rejects([&]{view.read_buffer(0,captured,1,0,0,4,result,0);});
  rejects([&]{view.read_buffer(0,captured,1,0,0,UINT64_MAX,result,1);});
  rejects([&]{view.read_buffer(0,captured,1,0,0,0,nullptr,1);});
  rejects([&]{view.read_buffer(NP,captured,1,0,0,0,result,1);});
  tracee_state closed=saved;
  auto descriptors=std::make_shared<FdManager>(closed.fd_manager_state); closed.fs_state.set_fd_manager(descriptors);
  for(int channel=0;channel<3;++channel) require(closed.fs_state.do_close(channel)==0,"close captured descriptor");
  closed.fd_manager_state=*descriptors;
  machine=save_stdio_fixture(closed,pages);
  for(bool exited:{false,true}) {
   machine.status[0]=exited?dstatus_exit(0):0;
   auto closed_view=cold(machine);
   for(int channel=0;channel<3;++channel) {
    require(closed_view.read_buffer(0,captured,channel,0,0,0,result,sizeof(result))==3 && result[0]=='0'+channel,"captured channel remains after close/exit");
    rejects([&]{closed_view.read_buffer(0,DETSIM_BUFFER_FILE,channel,0,0,0,result,1);});
   }
  }
  tracee_state redirected=saved;
  descriptors=std::make_shared<FdManager>(redirected.fd_manager_state); redirected.fs_state.set_fd_manager(descriptors);
  require(redirected.fs_state.do_close(1)==0,"close stdout before redirection");
  require(redirected.fs_state.do_open("/replacement",O_CREAT|O_RDWR,0600)==1,"reuse stdout descriptor");
  require(redirected.fs_state.do_write(1,"replacement",11)==11,"redirected file bytes");
  redirected.fd_manager_state=*descriptors;
  auto redirected_view=cold(save_stdio_fixture(redirected,pages));
  require(redirected_view.read_buffer(0,DETSIM_BUFFER_FILE,1,0,0,0,result,sizeof(result))==11 && std::string(result,11)=="replacement","FILE follows current fd");
  require(redirected_view.read_buffer(0,captured,1,0,0,0,result,sizeof(result))==3 && result[0]=='1' && result[1]=='\0',"captured channel independent of redirected fd");
  for(int corruption=0;corruption<4;++corruption) {
   tracee_state bad=closed;
   if(corruption==0) bad.fs_state.filesystem.erase(keys[1]);
   else if(corruption==1) bad.fs_state.filesystem.at(keys[1]).metadata.st_mode=S_IFREG|0600;
   else bad.fs_state.filesystem.at(keys[1]).metadata.st_size=corruption==2?-1:4;
   auto bad_view=cold(save_stdio_fixture(bad,pages));
   memset(result,'?',sizeof(result));
   rejects([&]{bad_view.read_buffer(0,captured,1,0,0,0,result,sizeof(result));});
   rejects([&]{bad_view.read_buffer(0,captured,1,0,0,0,nullptr,0);});
   require(result[0]=='?',"invalid captured channel must not copy fabricated bytes");
  }
  sys_state absent; auto absent_view=cold(absent);
  rejects([&]{absent_view.read_buffer(0,captured,1,0,0,0,result,1);});
 }
 puts("PASS captured stdio full/pages cold observation: close/exit, binary bytes, redirection, missing/type/size/selectors");
}
int main(int argc, char** argv) {
 require(argc==4,"expected two distinct DWARF binaries and changed-layout fixture");
 char directory[]="/tmp/detsim-view-XXXXXX";
 require(mkdtemp(directory),"mkdtemp");
 try {
  StateStore::Config cfg; cfg.packed_storage_path=std::string(directory)+"/packed"; cfg.hot_cache_size=1; cfg.warm_cache_size=1; cfg.prefetch_threads=0;
  auto& store=StateStore::instance(); store.init(cfg);
  captured_stdio_observation(store,cfg);
  RegionInfo r{}; r.start=0x1000; r.end=0x2000; r.prot=PROT_READ|PROT_WRITE; memcpy(r.flags,"rw-p",5);
  std::vector<uint8_t> bytes(4096); long value=123; memcpy(bytes.data(),&value,sizeof(value));
  auto page=store.save(bytes.data(),bytes.size());
  tracee_state saved; saved.tv={12,34};
  auto descriptors=std::make_shared<FdManager>(saved.fd_manager_state);
  saved.fs_state.set_fd_manager(descriptors);
  int file_fd=saved.fs_state.do_open("/captured",O_CREAT|O_RDWR|O_NONBLOCK|O_CLOEXEC,0600);
  require(file_fd>=0,"captured file"); require(saved.fs_state.do_write(file_fd,"abc",3)==3,"captured file bytes");
  int pipe_fds[2]; require(saved.fs_state.do_pipe(pipe_fds,O_NONBLOCK|O_CLOEXEC)==0,"captured pipe");
  const int pipe_id=saved.fs_state.pipe_fds_.at(pipe_fds[0]); saved.fs_state.pipes_.at(pipe_id).buffer={'p','i','p','e'};
  const int tcp_fd=descriptors->allocate_fd(FdType::SOCKET,O_RDWR|O_NONBLOCK,FD_CLOEXEC);
  const int udp_fd=descriptors->allocate_fd(FdType::SOCKET,O_RDWR,0);
  const int listener_fd=descriptors->allocate_fd(FdType::SOCKET,O_RDWR,0);
  const int epoll_fd=descriptors->allocate_fd(FdType::EPOLL,O_RDWR,FD_CLOEXEC);
  Socket stream; stream.fd=tcp_fd; stream.domain=AF_INET; stream.type=SOCK_STREAM; stream.connected=true;
  stream.local_addr.sin_family=AF_INET; stream.local_addr.sin_port=htons(1000); stream.local_addr.sin_addr.s_addr=htonl(0x7f000001);
  stream.peer_addr=stream.local_addr; stream.peer_addr.sin_port=htons(1001); stream.keep_alive=true;
  Socket datagram; datagram.fd=udp_fd; datagram.domain=AF_INET; datagram.type=SOCK_DGRAM;
  Socket listener; listener.fd=listener_fd; listener.domain=AF_INET; listener.type=SOCK_STREAM; listener.bound=true; listener.listening=true; listener.backlog=1;
  TcpConnection connection; connection.local_fd=tcp_fd; connection.local_addr=stream.local_addr; connection.peer_addr=stream.peer_addr; connection.recv_buffer={"tcp-payload",""};
  TcpConnection pending=connection; pending.local_fd=42; pending.recv_buffer={"pending-payload"};
  const uint64_t pending_key=(uint64_t(pending.local_addr.sin_addr.s_addr)<<32)|pending.local_addr.sin_port;
  listener.pending_connections.push_back(pending_key); listener.established_connections[pending_key]=pending;
  std::map<int,Socket> sockets{{tcp_fd,stream},{udp_fd,datagram},{listener_fd,listener}};
  UdpDatagram packet; packet.content="udp-payload"; packet.from=stream.peer_addr;
  std::map<int,std::deque<UdpDatagram>> udp{{udp_fd,{packet}}};
  std::map<int,TcpConnection> tcp{{tcp_fd,connection}};
  EpollInstance epoll; epoll.epoll_fd=epoll_fd; epoll.watched_fds[tcp_fd]={EPOLLIN,1234};
  std::map<int,EpollInstance> epolls{{epoll_fd,epoll}};
  std::ostringstream network_encoded; { cereal::BinaryOutputArchive archive(network_encoded); archive(sockets,udp,tcp,epolls); }
  std::istringstream network_input(network_encoded.str()); { cereal::BinaryInputArchive archive(network_input); archive(saved.sock_state); }
  saved.fd_manager_state=*descriptors;
  thread_state thread; thread.tid=1; thread.physical_tid=123456; thread.alive=true; thread.is_main=true; thread.regs.rax=987; thread.regs.rip=0x1234; saved.threads.push_back(thread);
  saved.futex_state.emplace();
  std::ostringstream encoded; { cereal::BinaryOutputArchive archive(encoded); archive(saved); }
  const auto serialized=encoded.str(); std::vector<uint8_t> metadata(serialized.begin(),serialized.end()); user_regs_struct regs{};
  auto data=encode_page_manifest({r},metadata,"maps",regs,{{r.start,4096,page}});
  sys_state machine; machine.ts_hash[0]=store.save(data.data(),data.size()); machine.child[0].tv={12,34};
  detsim::StateView view(machine);
  StateDataHeader header{}; header.magic=0x4453544D; header.version=FLAT_SNAPSHOT_VERSION; header.num_regions=1;
  header.tstate_offset=sizeof(header)+sizeof(r); header.tstate_size=metadata.size();
  header.maps_offset=header.tstate_offset+metadata.size(); header.maps_size=4;
  header.regs_offset=header.maps_offset+header.maps_size+bytes.size();
  std::vector<uint8_t> flat(header.regs_offset+sizeof(regs)); auto flat_region=r; flat_region.offset=header.maps_offset+header.maps_size;
  memcpy(flat.data(),&header,sizeof(header)); memcpy(flat.data()+sizeof(header),&flat_region,sizeof(r));
  memcpy(flat.data()+header.tstate_offset,metadata.data(),metadata.size()); memcpy(flat.data()+header.maps_offset,"maps",4);
  memcpy(flat.data()+flat_region.offset,bytes.data(),bytes.size()); memcpy(flat.data()+header.regs_offset,&regs,sizeof(regs));
  sys_state flat_machine=machine; flat_machine.ts_hash[0]=store.save(flat.data(),flat.size()); detsim::StateView flat_view(flat_machine);
  require(flat_view.evaluate(0,"*(long*)0x1000")==view.evaluate(0,"*(long*)0x1000"),"identical observation across storage backends");
  const auto metadata_json=view.inspect_json(0);
  require(metadata_json==flat_view.inspect_json(0),"identical canonical metadata across storage backends");
  const auto inspection=nlohmann::json::parse(metadata_json);
  require(inspection.at("schema")==1 && inspection.at("threads")[0].at("id")==1 && inspection.at("threads")[0].at("registers").at("rax")==987,"logical thread/register inspection");
  require(metadata_json.find("physical_tid")==std::string::npos && metadata_json.find("tcp-payload")==std::string::npos,"metadata excludes physical IDs and payloads");
  require(inspection.at("lifecycle")=="running" && inspection.at("exit_code").is_null(), "named lifecycle without private status headers");
  require(inspection.at("sockets")[0].at("options").at("keep_alive")==true,"captured socket options");
  require(inspection.at("sockets")[2].at("pending")[0]==std::to_string(pending_key),"endpoint keys use lossless decimal strings");
  char payload[32]{};
  require(view.read_buffer(0,DETSIM_BUFFER_FILE,file_fd,0,0,1,payload,sizeof(payload))==2 && std::string(payload,2)=="bc","file partial observation");
  require(view.read_buffer(0,DETSIM_BUFFER_FILE,file_fd,0,0,3,payload,sizeof(payload))==0,"file EOF");
  require(view.read_buffer(0,DETSIM_BUFFER_PIPE,pipe_fds[1],0,0,1,payload,2)==2 && std::string(payload,2)=="ip","pipe write endpoint observation");
  require(view.read_buffer(0,DETSIM_BUFFER_TCP,tcp_fd,0,0,0,payload,3)==3 && std::string(payload,3)=="tcp","TCP prefix");
  require(view.read_buffer(0,DETSIM_BUFFER_TCP,tcp_fd,0,1,0,payload,3)==0,"persistent EOF chunk");
  require(view.read_buffer(0,DETSIM_BUFFER_UDP,udp_fd,0,0,0,payload,3)==3 && std::string(payload,3)=="udp","UDP prefix");
  require(view.read_buffer(0,DETSIM_BUFFER_TCP_PENDING,listener_fd,pending_key,0,0,payload,7)==7 && std::string(payload,7)=="pending","preaccept prefix");
  rejects([&]{view.read_buffer(0,DETSIM_BUFFER_FILE,999,0,0,0,payload,1);});
  rejects([&]{view.read_buffer(0,DETSIM_BUFFER_FILE,file_fd,0,0,4,payload,1);});
  rejects([&]{view.read_buffer(0,DETSIM_BUFFER_UDP,udp_fd,0,9,0,payload,1);});
  rejects([&]{view.read_buffer(0,DETSIM_BUFFER_TCP_PENDING,listener_fd,0,0,0,payload,1);});
  rejects([&]{view.read_buffer(0,DETSIM_BUFFER_PIPE,file_fd,0,0,0,payload,1);});
  require(view.inspect_json(0)==metadata_json,"buffer observations do not consume queues or offsets");
  ptmc_state.fs_states[0]=FileSystemState(); ptmc_state.sock_states[0]=SockState(); machine.status[0]=1;
  require(view.inspect_json(0)==metadata_json,"saved resource observation independent of runtime mutation");
  ptmc_state.cursor=NP-1; ptmc_state.running_state.ts_hash[0]=0;
  long output=0; view.read(0,0x1000,&output,sizeof(output)); require(output==123,"saved read");
  require(view.evaluate(0,"*(long*)0x1000")==123,"saved expression independent of live state");
  require(view.evaluate(0,"tracee0(*(long*)0x1000)")==123,"qualified saved read");
  require(view.evaluate(0,"2 + 3 * 4")==14,"arithmetic");
  require(view.time(0).tv_sec==12 && view.time(0).tv_usec==34,"saved clock");
  sys_state cold_dead; cold_dead.ts_hash[0]=machine.ts_hash[0];
  detsim::StateView cold_dead_view(cold_dead);
  require(cold_dead_view.time(0).tv_sec==12 && cold_dead_view.time(0).tv_usec==34,"cold dead-node clock comes from saved metadata");
  rejects([&]{view.evaluate(0,"tracee999(1)");});
  for(const char* expression:{"*(long*)0x1000 = 7","++*(long*)0x1000","(*(long*)0x1000)++","0 && ++*(long*)0x1000","1 ? 2 : (*(long*)0x1000 = 7)","sizeof(++*(long*)0x1000)"}) rejects([&]{view.evaluate(0,expression);});
  rejects([&]{view.evaluate(0,"*(long*)0x3000");});
  rejects([&]{view.evaluate(0,"unknown_symbol");});
  rejects([&]{view.evaluate(0,"1 +");});
  rejects([&]{view.evaluate(0,"1 / 0");});
  rejects([&]{view.evaluate(0,"1 << 64");});
  rejects([&]{view.evaluate(0,"9223372036854775807 + 1");});
  rejects([&]{view.read(NP,0x1000,&output,sizeof(output));});
  view.read(0,0x1000,&output,sizeof(output)); require(output==123,"observation cannot mutate");
  machine.ts_hash[0]=0; machine.child[0].tv={99,0};
  require(view.evaluate(0,"*(long*)0x1000")==123 && view.time(0).tv_sec==12,"view binds immutable identity");
  sys_state missing; detsim::StateView unavailable(missing); rejects([&]{unavailable.evaluate(0,"*(long*)0x1000");});
  {
    // Both fixtures have the same named globals. The service must never apply
    // one binary's symbol/type table to the other configured logical node.
    ptmc_state.tracee[0].executable=argv[1]; ptmc_state.tracee[1].executable=argv[2];
    dwarf_init(argv[1]);
    require(dwarf_fixed_executable(),"non-PIE symbol fixture");
    const auto address=dwarf_get_global_addr("fixture_record"); require(address,"fixture global");
    auto symbolic_region=r; symbolic_region.start=address & ~uint64_t(4095); symbolic_region.end=symbolic_region.start+4096;
    auto symbol_bytes=bytes; memset(symbol_bytes.data(),0,symbol_bytes.size());
    memcpy(symbol_bytes.data()+address-symbolic_region.start+8,&value,sizeof(value));
    const auto symbol_page=store.save(symbol_bytes.data(),symbol_bytes.size());
    auto symbol_manifest=encode_page_manifest({symbolic_region},metadata,"maps",regs,{{symbolic_region.start,4096,symbol_page}});
    sys_state symbol_machine; symbol_machine.ts_hash[0]=store.save(symbol_manifest.data(),symbol_manifest.size()); symbol_machine.ts_hash[1]=symbol_machine.ts_hash[0];
    detsim::StateView symbolic(symbol_machine);
    require(symbolic.evaluate(0,"fixture_record.total")==123,"matching main binary symbolic observation");
    rejects([&]{symbolic.evaluate(1,"fixture_record.total");});
    rejects([&]{symbolic.evaluate(1,"sizeof(struct FixtureRecord)");});
    require(symbolic.evaluate(1,"2+3")==5,"mixed binary constants stay usable");
    symbolic.read(1,address+8,&output,sizeof(output)); require(output==123,"mixed binary raw read stays usable");
    dwarf_init(argv[2]); rejects([&]{symbolic.evaluate(0,"fixture_record.total");});
    dwarf_init(argv[3]); ptmc_state.tracee[0].executable=argv[3];
    const auto layout_address=dwarf_get_global_addr("fixture_record"); require(layout_address,"changed-layout global");
    auto layout_region=r; layout_region.start=layout_address & ~uint64_t(4095); layout_region.end=layout_region.start+4096;
    memset(symbol_bytes.data(),0,symbol_bytes.size());
    long layout_value=456; memcpy(symbol_bytes.data()+layout_address-layout_region.start+16,&layout_value,sizeof(layout_value));
    auto layout_page=store.save(symbol_bytes.data(),symbol_bytes.size());
    auto layout_manifest=encode_page_manifest({layout_region},metadata,"maps",regs,{{layout_region.start,4096,layout_page}});
    sys_state layout_machine; layout_machine.ts_hash[0]=store.save(layout_manifest.data(),layout_manifest.size());
    detsim::StateView layout_view(layout_machine);
    require(layout_view.evaluate(0,"fixture_record.total")==456,"symbol reload invalidates cached same-name member layout");
    dwarf_cleanup();
    ptmc_state.tracee[0].executable=const_cast<char*>("/proc/self/exe");
    dwarf_init("/proc/self/exe");
    detsim::StateView own_image(symbol_machine);
    if (!dwarf_fixed_executable()) rejects([&]{own_image.evaluate(0,"sizeof(struct FixtureRecord)");});
    require(own_image.evaluate(0,"*(long*)"+std::to_string(address+8))==123,"raw casts do not need symbolic layout");
    dwarf_cleanup();
  }
  ptmc_state.fd_managers[0]=descriptors; ptmc_state.fs_states[0]=saved.fs_state; ptmc_state.sock_states[0]=saved.sock_state;
  auto live=detsim::StateView::stopped_live(); const auto live_json=live.inspect_json(0);
  require(!nlohmann::json::parse(live_json).at("registers_available").get<bool>() && nlohmann::json::parse(live_json).at("threads").is_null(),"live registers explicitly unavailable");
  ptmc_state.fs_states[0]=FileSystemState(); ptmc_state.sock_states[0]=SockState();
  require(live.inspect_json(0)==live_json,"live metadata query binds a consistent private copy");
  store.shutdown(); rejects([&]{view.evaluate(0,"*(long*)0x1000");});
  rejects([&]{view.inspect_json(0);}); rejects([&]{view.time(0);});
  rejects([&]{view.read_buffer(0,DETSIM_BUFFER_FILE,file_fd,0,0,0,payload,1);});
  store.init(cfg); rejects([&]{view.inspect_json(0);}); rejects([&]{view.time(0);});
  require(detsim::StateView(flat_machine).inspect_json(0)==metadata_json,"new view after reopen retains canonical metadata");
  store.shutdown();
  std::filesystem::remove_all(directory); puts("PASS read-only state observation"); return 0;
 } catch(const std::exception& e) { fprintf(stderr,"state view: %s\n",e.what()); StateStore::instance().shutdown(); std::filesystem::remove_all(directory); return 1; }
}
