#include "state_view.h"
#include "page_snapshot.h"
#include "guest.h"
#include "monitor.h"
#include "proc_status.h"
#include <stdexcept>
#include "dwarf_info.h"
#include <filesystem>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <iterator>
namespace detsim {
StateView::StateView(const sys_state& state) {
  store_generation_=StateStore::instance().snapshot_generation();
  for (size_t i=0; i<NP; ++i) {
    executables_[i]=ptmc_state.tracee[i].executable ? ptmc_state.tracee[i].executable : "";
    snapshots_[i]=state.ts_hash[i];
    statuses_[i]=state.status[i];
    // Saved metadata is authoritative, including nodes omitted by cold system loading.
  }
}
StateView StateView::stopped_live() {
  StateView result;
  result.live_=true;
  for (size_t i=0; i<NP; ++i) {
    result.executables_[i]=ptmc_state.tracee[i].executable ? ptmc_state.tracee[i].executable : "";
    result.live_pids_[i]=ptmc_state.pids[i];
    result.statuses_[i]=ptmc_state.status[i];
    result.clocks_[i]=ptmc_state.time[i];

  }
  return result;
}
void StateView::read(size_t node, uint64_t address, void* destination, size_t length) const {
  if (node>=NP) throw std::out_of_range("Observation process index");
  if (live_) {
    if (!guest_read_exact(live_pids_[node], reinterpret_cast<const void*>(address), destination, length))
      throw std::runtime_error("Cannot read stopped guest memory");
  } else {
    check_snapshot_lifetime();
    if (!snapshots_[node]) throw std::runtime_error("Observation has no saved process snapshot");
    SnapshotView::open(snapshots_[node])->read_into(address, destination, length);
  }
}
void StateView::require_symbols(size_t node) const {
  if (node>=NP) throw std::out_of_range("Observation process index");
  const auto& configured = executables_[node];
  const auto& loaded = dwarf_executable_path();
  std::error_code error;
  if (configured.empty() || loaded.empty() ||
      (configured != loaded && !std::filesystem::equivalent(configured, loaded, error)))
    throw std::runtime_error("Observation symbols do not match the configured node executable");
  if (!dwarf_fixed_executable())
    throw std::runtime_error("Observation of PIE/shared-image symbols requires per-node relocation support");
}
void StateView::require_symbol_address(size_t node, uint64_t address) const {
  require_symbols(node);
  if (!dwarf_main_image_contains(address))
    throw std::runtime_error("Observation cannot use symbols outside the configured fixed executable");
}
int64_t StateView::evaluate(size_t node, const std::string& expression) const {
  return evaluate_observation(*this, node, expression);
}
void StateView::check_snapshot_lifetime() const {
  if (!live_ && StateStore::instance().generation()!=store_generation_)
    throw std::runtime_error("Observation belongs to a closed or failed snapshot store");
}
const tracee_state& StateView::metadata(size_t node) const {
  check_snapshot_lifetime();
  if (node>=NP) throw std::out_of_range("Observation process index");
  if (!metadata_[node]) {
    if (live_) {
      auto state=std::make_shared<tracee_state>();
      state->tv=clocks_[node];
      state->fs_state=ptmc_state.fs_states[node];
      state->sock_state=ptmc_state.sock_states[node];
      state->process_state=ptmc_state.process_states[node];
      if (!ptmc_state.fd_managers[node]) throw std::runtime_error("Missing live descriptor namespace");
      state->fd_manager_state=*ptmc_state.fd_managers[node];
      metadata_[node]=std::move(state);
    } else {
      if (!snapshots_[node]) throw std::runtime_error("Observation has no saved process snapshot");
      metadata_[node]=std::make_shared<tracee_state>(snapshots_[node]);
    }
  }
  return *metadata_[node];
}
timeval StateView::time(size_t node) const {
  if (node>=NP) throw std::out_of_range("Observation process index");
  return live_ ? clocks_[node] : metadata(node).tv;
}
namespace {
using Json=nlohmann::json;
Json endpoint(const sockaddr_in& address) {
  return {{"family",address.sin_family},{"ipv4",ntohl(address.sin_addr.s_addr)},{"port",ntohs(address.sin_port)}};
}
Json queue_lengths(const std::deque<std::string>& queue) {
  auto result=Json::array();
  for (const auto& chunk:queue) result.push_back({{"length",chunk.size()},{"eof",chunk.empty()}});
  return result;
}
const char* fd_kind(FdType kind) {
  switch (kind) {
    case FdType::FILE:return "file"; case FdType::SOCKET:return "socket";
    case FdType::EPOLL:return "epoll"; case FdType::PIPE:return "pipe";
    case FdType::STDIO:return "stdio"; case FdType::UNKNOWN:return "unknown";
  }
  throw std::runtime_error("Unknown descriptor kind");
}
Json registers(const user_regs_struct& registers) {
  Json result=Json::object();
#define REG(name) result[#name]=registers.name
  REG(r15); REG(r14); REG(r13); REG(r12); REG(rbp); REG(rbx);
  REG(r11); REG(r10); REG(r9); REG(r8); REG(rax); REG(rcx); REG(rdx);
  REG(rsi); REG(rdi); REG(orig_rax); REG(rip); REG(cs); REG(eflags);
  REG(rsp); REG(ss); REG(fs_base); REG(gs_base); REG(ds); REG(es); REG(fs); REG(gs);
#undef REG
  return result;
}
}
std::string StateView::inspect_json(size_t node) const {
  const auto& state=metadata(node);
  const auto& fs=state.fs_state;
  const auto& network=state.sock_state;
  const auto& manager=state.fd_manager_state;
  Json result={{"schema",1},{"process",node},{"status",statuses_[node]},{"clock",{{"seconds",state.tv.tv_sec},{"microseconds",state.tv.tv_usec}}},
    {"cwd",fs.cwd},{"registers_available",!live_},{"futex_available",!live_}};
  const int status = statuses_[node];
  result["lifecycle"] = DISEXITED(status) ? "exited" : DISCRASHED(status) ? "crashed" :
      DISSIGNALED(status) ? "signaled" : (status & DIFSTOPPED) ? "stopped" : "running";
  result["exit_code"] = DISEXITED(status) ? Json(DEXITSTATUS(status)) : Json(nullptr);
  result["signal"] = (status & (DIFCRASHED | DIFSIGNALED | DIFSTOPPED)) ? Json(DSTOPSIG(status)) : Json(nullptr);
  result["creation_mask"] = fs.creation_mask();
  result["kernel_process"] = {{"random_state",state.process_state.random_state},{"signal_actions",Json::array()}};
  for(const auto& [signal,action]:state.process_state.signal_actions)
    result["kernel_process"]["signal_actions"].push_back({{"signal",signal},{"handler",action.handler},
      {"flags",action.flags},{"restorer",action.restorer},{"mask",action.mask}});
  auto descriptors=Json::array(); auto fds=manager.get_allocated_fds();
  std::vector<int> sorted(fds.begin(),fds.end()); std::sort(sorted.begin(),sorted.end());
  for (int fd:sorted) descriptors.push_back({{"fd",fd},{"kind",fd_kind(manager.get_fd_type(fd))},
    {"status_flags",manager.get_status_flags(fd)},{"descriptor_flags",manager.get_descriptor_flags(fd)}});
  result["descriptors"]=std::move(descriptors);
  auto files=Json::array();
  for (const auto& [path,file]:fs.filesystem) files.push_back({{"path",path},{"mode",file.metadata.st_mode},{"size",file.metadata.st_size},{"buffer_length",file.content.size()}});
  result["files"]=std::move(files);
  auto open_files=Json::array();
  for (const auto& [fd,file]:fs.open_files) open_files.push_back({{"fd",fd},{"path",file.path},{"offset",file.offset}});
  result["open_files"]=std::move(open_files);
  auto pipes=Json::array();
  for (const auto& [id,pipe]:fs.pipes_) pipes.push_back({{"id",id},{"length",pipe.buffer.size()},{"read_closed",pipe.read_closed},{"write_closed",pipe.write_closed}});
  result["pipes"]=std::move(pipes);
  auto pipe_fds=Json::array();
  for (const auto& [fd,id]:fs.pipe_fds_) pipe_fds.push_back({{"fd",fd},{"pipe",id>0?id:-id},{"direction",id>0?"read":"write"}});
  result["pipe_endpoints"]=std::move(pipe_fds);
  auto sockets=Json::array();
  for (const auto& [fd,socket]:network.sockets()) {
    Json item={{"fd",fd},{"domain",socket.domain},{"type",socket.type},{"protocol",socket.protocol},
      {"bound",socket.bound},{"connected",socket.connected},{"listening",socket.listening},
      {"local",endpoint(socket.local_addr)},{"peer",endpoint(socket.peer_addr)},{"backlog",socket.backlog},
      {"pending",Json::array()},{"options",{{"reuse_address",socket.reuse_address},{"no_delay",socket.no_delay},
        {"keep_alive",socket.keep_alive},{"keep_idle",socket.keep_idle},{"keep_interval",socket.keep_interval},{"keep_count",socket.keep_count}}}};
    for (const auto key:socket.pending_connections) item["pending"].push_back(std::to_string(key));
    auto pending=Json::array();
    for (const auto& [key,connection]:socket.established_connections) pending.push_back({{"key",std::to_string(key)},{"local",endpoint(connection.local_addr)},{"peer",endpoint(connection.peer_addr)},{"queue",queue_lengths(connection.recv_buffer)}});
    item["pending_connections"]=std::move(pending); sockets.push_back(std::move(item));
  }
  result["sockets"]=std::move(sockets);
  auto tcp=Json::array();
  for (const auto& [fd,connection]:network.tcp_connections()) tcp.push_back({{"fd",fd},{"local",endpoint(connection.local_addr)},{"peer",endpoint(connection.peer_addr)},{"queue",queue_lengths(connection.recv_buffer)}});
  result["tcp"]=std::move(tcp);
  auto udp=Json::array();
  for (const auto& [fd,queue]:network.udp_recv_buffers()) {
    auto messages=Json::array(); for (const auto& message:queue) messages.push_back({{"length",message.content.size()},{"from",endpoint(message.from)}});
    udp.push_back({{"fd",fd},{"queue",std::move(messages)}});
  }
  result["udp"]=std::move(udp);
  auto epolls=Json::array();
  for (const auto& [fd,instance]:network.epoll_instances()) {
    auto watches=Json::array(); for (const auto& [watched,watch]:instance.watched_fds) watches.push_back({{"fd",watched},{"events",watch.events},{"data",watch.data}});
    epolls.push_back({{"fd",fd},{"watches",std::move(watches)}});
  }
  result["epolls"]=std::move(epolls);
  result["threads"]=live_ ? Json(nullptr) : Json::array();
  if (!live_) for (const auto& thread:state.threads) result["threads"].push_back({{"id",thread.tid},{"alive",thread.alive},{"is_main",thread.is_main},{"registers",registers(thread.regs)}});
  result["futex"]=live_ ? Json(nullptr) : Json::array();
  if (!live_ && state.futex_state) for (const auto& [address,queue]:state.futex_state->wait_queues()) {
    auto waiters=Json::array(); for (const auto& waiter:queue) waiters.push_back({{"thread",waiter.tid},{"value",waiter.val},{"bitset",waiter.bitset},{"timeout",waiter.timeout}});
    result["futex"].push_back({{"address",address},{"waiters",std::move(waiters)}});
  }
  return result.dump();
}
size_t StateView::read_buffer(size_t node, detsim_buffer_kind kind, int fd,
    uint64_t item, uint64_t message, uint64_t offset, void* destination, size_t length) const {
  if (length && !destination) throw std::invalid_argument("Null buffer destination");
  const auto& state=metadata(node);
  const auto type=state.fd_manager_state.get_fd_type(fd);
  auto check=[](bool valid) { if (!valid) throw std::runtime_error("Unavailable resource buffer selector"); };
  auto copy=[&](const auto& source)->size_t {
    check(offset<=source.size());
    const size_t count=std::min<size_t>(length,source.size()-size_t(offset));
    if (count) std::copy_n(std::next(source.begin(),size_t(offset)),count,static_cast<char*>(destination));
    return count;
  };
  if (kind==DETSIM_BUFFER_FILE) {
    check(!item && !message && (type==FdType::FILE || type==FdType::STDIO));
    const auto file=state.fs_state.open_files.find(fd); check(file!=state.fs_state.open_files.end());
    const auto data=state.fs_state.filesystem.find(file->second.path); check(data!=state.fs_state.filesystem.end());
    return copy(data->second.content);
  }
  if (kind==DETSIM_BUFFER_PIPE) {
    check(!item && !message && type==FdType::PIPE);
    const auto endpoint=state.fs_state.pipe_fds_.find(fd); check(endpoint!=state.fs_state.pipe_fds_.end());
    const auto pipe=state.fs_state.pipes_.find(endpoint->second>0?endpoint->second:-endpoint->second); check(pipe!=state.fs_state.pipes_.end());
    return copy(pipe->second.buffer);
  }
  check(type==FdType::SOCKET);
  const auto socket=state.sock_state.sockets().find(fd); check(socket!=state.sock_state.sockets().end());
  if (kind==DETSIM_BUFFER_UDP) {
    check(!item && socket->second.type==SOCK_DGRAM);
    const auto queue=state.sock_state.udp_recv_buffers().find(fd); check(queue!=state.sock_state.udp_recv_buffers().end());
    check(message<queue->second.size()); return copy(queue->second[size_t(message)].content);
  }
  check(socket->second.type==SOCK_STREAM);
  if (kind==DETSIM_BUFFER_TCP) {
    check(!item); const auto connection=state.sock_state.tcp_connections().find(fd); check(connection!=state.sock_state.tcp_connections().end());
    check(message<connection->second.recv_buffer.size()); return copy(connection->second.recv_buffer[size_t(message)]);
  }
  if (kind==DETSIM_BUFFER_TCP_PENDING) {
    check(socket->second.listening);
    const auto connection=socket->second.established_connections.find(item); check(connection!=socket->second.established_connections.end());
    check(message<connection->second.recv_buffer.size()); return copy(connection->second.recv_buffer[size_t(message)]);
  }
  throw std::invalid_argument("Unknown buffer kind");
}
}
