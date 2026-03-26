/*
 * Raft Message Parser Implementation
 */

#include "raft_msg_parser.h"
#include <cstring>
#include <cstdio>
#include <sstream>
#include <iomanip>

namespace raft {

const char* get_msg_type_name(int type) {
    switch (type) {
        case RAFT_MSG_APPENDENTRIES:          return "APPEND_ENTRIES";
        case RAFT_MSG_APPENDENTRIES_RESPONSE: return "APPEND_ENTRIES_RSP";
        case RAFT_MSG_REQUESTVOTE:            return "REQUEST_VOTE";
        case RAFT_MSG_REQUESTVOTE_RESPONSE:   return "REQUEST_VOTE_RSP";
        case RAFT_MSG_ENTRY:                  return "ENTRY";
        case RAFT_MSG_ENTRY_RESPONSE:         return "ENTRY_RSP";
        case RAFT_MSG_SNAPSHOT:               return "SNAPSHOT";
        case RAFT_MSG_SNAPSHOT_RESPONSE:      return "SNAPSHOT_RSP";
        default:                              return "UNKNOWN";
    }
}

// Helper to read values from buffer
template<typename T>
static bool read_value(const uint8_t*& ptr, size_t& remaining, T& out) {
    if (remaining < sizeof(T)) return false;
    memcpy(&out, ptr, sizeof(T));
    ptr += sizeof(T);
    remaining -= sizeof(T);
    return true;
}

std::string parse_requestvote(const void* buf, size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(buf);
    size_t remaining = len;
    
    msg_requestvote_t msg;
    if (!read_value(ptr, remaining, msg.term)) return "REQUEST_VOTE[malformed]";
    if (!read_value(ptr, remaining, msg.candidate_id)) return "REQUEST_VOTE[malformed]";
    // Skip 4 bytes padding that exists in C struct after candidate_id
    if (remaining < 4) return "REQUEST_VOTE[malformed]";
    ptr += 4;
    remaining -= 4;
    if (!read_value(ptr, remaining, msg.last_log_idx)) return "REQUEST_VOTE[malformed]";
    if (!read_value(ptr, remaining, msg.last_log_term)) return "REQUEST_VOTE[malformed]";
    // Skip 4 bytes padding between msg struct and sender
    if (remaining < 4) return "REQUEST_VOTE[malformed]";
    ptr += 4;
    remaining -= 4;
    
    // Read sender id if present
    int sender = -1;
    if (remaining >= 4) {
        read_value(ptr, remaining, sender);
    }
    
    std::ostringstream oss;
    oss << "REQUEST_VOTE{term=" << msg.term 
        << ", cand=" << msg.candidate_id
        << ", last_idx=" << msg.last_log_idx
        << ", last_term=" << msg.last_log_term;
    if (sender >= 0) oss << ", from=" << sender;
    oss << "}";
    return oss.str();
}

std::string parse_requestvote_response(const void* buf, size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(buf);
    size_t remaining = len;
    
    msg_requestvote_response_t msg;
    if (!read_value(ptr, remaining, msg.term)) return "REQUEST_VOTE_RSP[malformed]";
    if (!read_value(ptr, remaining, msg.vote_granted)) return "REQUEST_VOTE_RSP[malformed]";
    // Skip 4 bytes padding that exists in C struct after vote_granted
    if (remaining < 4) return "REQUEST_VOTE_RSP[malformed]";
    ptr += 4;
    remaining -= 4;
    
    // Read sender id if present
    int sender = -1;
    if (remaining >= 4) {
        read_value(ptr, remaining, sender);
    }
    
    std::ostringstream oss;
    oss << "REQUEST_VOTE_RSP{term=" << msg.term 
        << ", granted=" << (msg.vote_granted ? "Y" : "N");
    if (sender >= 0) oss << ", from=" << sender;
    oss << "}";
    return oss.str();
}

std::string parse_appendentries(const void* buf, size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(buf);
    size_t remaining = len;
    
    msg_appendentries_t msg;
    if (!read_value(ptr, remaining, msg.term)) return "APPEND_ENTRIES[malformed]";
    if (!read_value(ptr, remaining, msg.prev_log_idx)) return "APPEND_ENTRIES[malformed]";
    if (!read_value(ptr, remaining, msg.prev_log_term)) return "APPEND_ENTRIES[malformed]";
    if (!read_value(ptr, remaining, msg.leader_commit)) return "APPEND_ENTRIES[malformed]";
    if (!read_value(ptr, remaining, msg.n_entries)) return "APPEND_ENTRIES[malformed]";
    
    // Skip padding that exists in C struct (4 bytes on 64-bit systems)
    // This padding exists between n_entries and the entries pointer in the original C struct
    int padding;
    if (!read_value(ptr, remaining, padding)) return "APPEND_ENTRIES[malformed]";
    
    std::ostringstream oss;
    oss << "APPEND_ENTRIES{term=" << msg.term 
        << ", prev_idx=" << msg.prev_log_idx
        << ", prev_term=" << msg.prev_log_term
        << ", commit=" << msg.leader_commit
        << ", n_ent=" << msg.n_entries;
    
    // Parse entries if present
    for (int i = 0; i < msg.n_entries && remaining >= sizeof(raft_term_t) + sizeof(int) * 2 + sizeof(unsigned int); i++) {
        raft_term_t entry_term;
        int entry_id, entry_type;
        unsigned int data_len;
        
        if (!read_value(ptr, remaining, entry_term)) break;
        if (!read_value(ptr, remaining, entry_id)) break;
        if (!read_value(ptr, remaining, entry_type)) break;
        if (!read_value(ptr, remaining, data_len)) break;
        
        oss << " [entry" << i << "={term=" << entry_term << ",id=" << entry_id << "}]";
        
        // Skip entry data
        if (remaining < data_len) break;
        ptr += data_len;
        remaining -= data_len;
    }
    
    oss << "}";
    return oss.str();
}

std::string parse_appendentries_response(const void* buf, size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(buf);
    size_t remaining = len;
    
    msg_appendentries_response_t msg;
    if (!read_value(ptr, remaining, msg.term)) return "APPEND_ENTRIES_RSP[malformed]";
    if (!read_value(ptr, remaining, msg.success)) return "APPEND_ENTRIES_RSP[malformed]";
    // Skip 4 bytes padding that exists in C struct after success
    if (remaining < 4) return "APPEND_ENTRIES_RSP[malformed]";
    ptr += 4;
    remaining -= 4;
    if (!read_value(ptr, remaining, msg.current_idx)) return "APPEND_ENTRIES_RSP[malformed]";
    if (!read_value(ptr, remaining, msg.first_idx)) return "APPEND_ENTRIES_RSP[malformed]";
    // Skip padding at end of struct (if any)
    // Note: AER doesn't have sender field in main.c, but we need to account for struct padding
    
    std::ostringstream oss;
    oss << "APPEND_ENTRIES_RSP{term=" << msg.term 
        << ", success=" << (msg.success ? "Y" : "N")
        << ", cur_idx=" << msg.current_idx
        << ", first_idx=" << msg.first_idx
        << "}";
    return oss.str();
}

std::string parse_snapshot(const void* buf, size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(buf);
    size_t remaining = len;
    
    raft_index_t snap_idx;
    raft_term_t snap_term;
    int snap_len;
    
    if (!read_value(ptr, remaining, snap_idx)) return "SNAPSHOT[malformed]";
    if (!read_value(ptr, remaining, snap_term)) return "SNAPSHOT[malformed]";
    if (!read_value(ptr, remaining, snap_len)) return "SNAPSHOT[malformed]";
    
    std::ostringstream oss;
    oss << "SNAPSHOT{idx=" << snap_idx 
        << ", term=" << snap_term
        << ", len=" << snap_len
        << "}";
    return oss.str();
}

std::string parse_entry(const void* buf, size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(buf);
    size_t remaining = len;
    
    // entry message format: type(4) + entry_data
    raft_term_t term;
    int id, type;
    unsigned int data_len;
    
    if (!read_value(ptr, remaining, term)) return "ENTRY[malformed]";
    if (!read_value(ptr, remaining, id)) return "ENTRY[malformed]";
    if (!read_value(ptr, remaining, type)) return "ENTRY[malformed]";
    if (!read_value(ptr, remaining, data_len)) return "ENTRY[malformed]";
    
    std::ostringstream oss;
    oss << "ENTRY{term=" << term 
        << ", id=" << id
        << ", type=" << type
        << ", len=" << data_len
        << "}";
    return oss.str();
}

std::string parse_entry_response(const void* buf, size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(buf);
    size_t remaining = len;
    
    int id;
    raft_term_t term;
    raft_index_t idx;
    
    if (!read_value(ptr, remaining, id)) return "ENTRY_RSP[malformed]";
    if (!read_value(ptr, remaining, term)) return "ENTRY_RSP[malformed]";
    if (!read_value(ptr, remaining, idx)) return "ENTRY_RSP[malformed]";
    
    std::ostringstream oss;
    oss << "ENTRY_RSP{id=" << id 
        << ", term=" << term
        << ", idx=" << idx
        << "}";
    return oss.str();
}

std::string parse_raft_message(const void* buf, size_t len) {
    if (len < 4) {
        return "RAFT[too_short]";
    }
    
    int msg_type;
    memcpy(&msg_type, buf, sizeof(int));
    
    // Skip type field for parsing
    const uint8_t* msg_start = static_cast<const uint8_t*>(buf) + 4;
    size_t msg_len = len - 4;
    
    switch (msg_type) {
        case RAFT_MSG_APPENDENTRIES:
            return parse_appendentries(msg_start, msg_len);
        case RAFT_MSG_APPENDENTRIES_RESPONSE:
            return parse_appendentries_response(msg_start, msg_len);
        case RAFT_MSG_REQUESTVOTE:
            return parse_requestvote(msg_start, msg_len);
        case RAFT_MSG_REQUESTVOTE_RESPONSE:
            return parse_requestvote_response(msg_start, msg_len);
        case RAFT_MSG_ENTRY:
            return parse_entry(msg_start, msg_len);
        case RAFT_MSG_ENTRY_RESPONSE:
            return parse_entry_response(msg_start, msg_len);
        case RAFT_MSG_SNAPSHOT:
            return parse_snapshot(msg_start, msg_len);
        case RAFT_MSG_SNAPSHOT_RESPONSE:
            return "SNAPSHOT_RSP{}";
        default:
            std::ostringstream oss;
            oss << "RAFT{type=" << msg_type << ",len=" << len << "}";
            return oss.str();
    }
}

} // namespace raft
