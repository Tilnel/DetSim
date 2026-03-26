/*
 * Raft Message Parser
 *
 * Parses Raft protocol messages for display in tracer
 * Supports message types from redisraft library
 */

#ifndef RAFT_MSG_PARSER_H
#define RAFT_MSG_PARSER_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace raft
{
using raft_term_t = long;
using raft_index_t = long;
using raft_node_id_t = int;
using raft_entry_id_t = long;
using raft_session_t = unsigned long long;
using raft_size_t = size_t;

enum RaftMsgType
{
  RAFT_MSG_APPENDENTRIES = 0,
  RAFT_MSG_APPENDENTRIES_RESPONSE,
  RAFT_MSG_REQUESTVOTE,
  RAFT_MSG_REQUESTVOTE_RESPONSE,
  RAFT_MSG_ENTRY,
  RAFT_MSG_ENTRY_RESPONSE,
  RAFT_MSG_SNAPSHOT,
  RAFT_MSG_SNAPSHOT_RESPONSE
};

struct raft_requestvote_req_t
{
  int prevote;
  raft_term_t term;
  raft_node_id_t candidate_id;
  raft_index_t last_log_idx;
  raft_term_t last_log_term;
};

struct raft_requestvote_resp_t
{
  int prevote;
  raft_term_t request_term;
  raft_term_t term;
  int vote_granted;
};

struct raft_appendentries_req_t
{
  raft_node_id_t leader_id;
  unsigned long msg_id;
  raft_term_t term;
  raft_index_t prev_log_idx;
  raft_term_t prev_log_term;
  raft_index_t leader_commit;
  raft_index_t n_entries;
};

struct raft_appendentries_resp_t
{
  unsigned long msg_id;
  raft_term_t term;
  int success;
  raft_index_t current_idx;
};

std::string parse_raft_message(const void *buf, size_t len);
const char *get_msg_type_name(int type);
std::string parse_requestvote(const void *buf, size_t len);
std::string parse_requestvote_response(const void *buf, size_t len);
std::string parse_appendentries(const void *buf, size_t len);
std::string parse_appendentries_response(const void *buf, size_t len);
std::string parse_snapshot(const void *buf, size_t len);

} // namespace raft

#endif
