/*
 * Raft Message Parser
 * 
 * Parses Raft protocol messages for display in tracer
 * Supports message types from willemt/raft library
 */

#ifndef RAFT_MSG_PARSER_H
#define RAFT_MSG_PARSER_H

#include <cstdint>
#include <cstddef>
#include <string>

namespace raft {

// Message type constants (from main.c)
enum RaftMsgType {
    RAFT_MSG_APPENDENTRIES = 0,
    RAFT_MSG_APPENDENTRIES_RESPONSE,
    RAFT_MSG_REQUESTVOTE,
    RAFT_MSG_REQUESTVOTE_RESPONSE,
    RAFT_MSG_ENTRY,
    RAFT_MSG_ENTRY_RESPONSE,
    RAFT_MSG_SNAPSHOT,
    RAFT_MSG_SNAPSHOT_RESPONSE
};

// Type aliases (from raft_types.h)
using raft_term_t = long;
using raft_index_t = long;
using raft_node_id_t = int;

// Message structures (from raft.h)
struct msg_requestvote_t {
    raft_term_t term;
    raft_node_id_t candidate_id;
    // Note: C struct has 4 bytes padding here (offset 12-15)
    raft_index_t last_log_idx;
    raft_term_t last_log_term;
};

struct msg_requestvote_response_t {
    raft_term_t term;
    int vote_granted;
    // Note: C struct has 4 bytes padding here (offset 12-15)
};

struct msg_appendentries_t {
    raft_term_t term;
    raft_index_t prev_log_idx;
    raft_term_t prev_log_term;
    raft_index_t leader_commit;
    int n_entries;
    // Explicit padding to match C struct layout (entries pointer follows)
    int padding;
    // entries pointer is not serialized directly
};

struct msg_appendentries_response_t {
    raft_term_t term;
    int success;
    // Note: C struct has 4 bytes padding here (offset 12-15)
    raft_index_t current_idx;
    raft_index_t first_idx;
};

struct raft_entry_data_t {
    void* buf;
    unsigned int len;
};

struct raft_entry_t {
    raft_term_t term;
    int id;
    int type;
    raft_entry_data_t data;
};

// Parse a Raft message from buffer
// Returns human-readable description
std::string parse_raft_message(const void* buf, size_t len);

// Get message type name
const char* get_msg_type_name(int type);

// Parse specific message types
std::string parse_requestvote(const void* buf, size_t len);
std::string parse_requestvote_response(const void* buf, size_t len);
std::string parse_appendentries(const void* buf, size_t len);
std::string parse_appendentries_response(const void* buf, size_t len);
std::string parse_snapshot(const void* buf, size_t len);
std::string parse_entry(const void* buf, size_t len);
std::string parse_entry_response(const void* buf, size_t len);

} // namespace raft

#endif // RAFT_MSG_PARSER_H
