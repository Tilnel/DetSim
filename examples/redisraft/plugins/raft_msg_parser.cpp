#include "raft_msg_parser.h"
#include <cstdio>
#include <cstring>
#include <sstream>

namespace raft
{

const char *get_msg_type_name(int type)
{
  switch (type)
  {
    case RAFT_MSG_APPENDENTRIES:
      return "APPEND_ENTRIES";
    case RAFT_MSG_APPENDENTRIES_RESPONSE:
      return "APPEND_ENTRIES_RSP";
    case RAFT_MSG_REQUESTVOTE:
      return "REQUEST_VOTE";
    case RAFT_MSG_REQUESTVOTE_RESPONSE:
      return "REQUEST_VOTE_RSP";
    case RAFT_MSG_ENTRY:
      return "ENTRY";
    case RAFT_MSG_ENTRY_RESPONSE:
      return "ENTRY_RSP";
    case RAFT_MSG_SNAPSHOT:
      return "SNAPSHOT";
    case RAFT_MSG_SNAPSHOT_RESPONSE:
      return "SNAPSHOT_RSP";
    default:
      return "UNKNOWN";
  }
}

template <typename T>
static bool read_value(const uint8_t *&ptr, size_t &remaining, T &out)
{
  if (remaining < sizeof(T))
    return false;
  memcpy(&out, ptr, sizeof(T));
  ptr += sizeof(T);
  remaining -= sizeof(T);
  return true;
}

static bool skip_bytes(const uint8_t *&ptr, size_t &remaining, size_t n)
{
  if (remaining < n)
    return false;
  ptr += n;
  remaining -= n;
  return true;
}

std::string parse_requestvote(const void *buf, size_t len)
{
  const uint8_t *ptr = static_cast<const uint8_t *>(buf);
  size_t remaining = len;

  // C struct layout with padding: prevote(4) + pad(4) + term(8) +
  // candidate_id(4) + pad(4) + last_log_idx(8) + last_log_term(8)
  raft_requestvote_req_t msg;
  if (!read_value(ptr, remaining, msg.prevote))
    return "REQUEST_VOTE[malformed]";
  if (!skip_bytes(ptr, remaining, 4)) // padding after prevote
    return "REQUEST_VOTE[malformed]";
  if (!read_value(ptr, remaining, msg.term))
    return "REQUEST_VOTE[malformed]";
  if (!read_value(ptr, remaining, msg.candidate_id))
    return "REQUEST_VOTE[malformed]";
  if (!skip_bytes(ptr, remaining, 4)) // padding after candidate_id
    return "REQUEST_VOTE[malformed]";
  if (!read_value(ptr, remaining, msg.last_log_idx))
    return "REQUEST_VOTE[malformed]";
  if (!read_value(ptr, remaining, msg.last_log_term))
    return "REQUEST_VOTE[malformed]";

  int sender = -1;
  if (remaining >= 4)
  {
    read_value(ptr, remaining, sender);
  }

  std::ostringstream oss;
  oss << "REQUEST_VOTE{term=" << msg.term << ", cand=" << msg.candidate_id
      << ", last_idx=" << msg.last_log_idx
      << ", last_term=" << msg.last_log_term;
  if (sender >= 0)
    oss << ", from=" << sender;
  if (msg.prevote)
    oss << ", prevote=1";
  oss << "}";
  return oss.str();
}

std::string parse_requestvote_response(const void *buf, size_t len)
{
  const uint8_t *ptr = static_cast<const uint8_t *>(buf);
  size_t remaining = len;

  // C struct layout with padding: prevote(4) + pad(4) + request_term(8) +
  // term(8) + vote_granted(4)
  raft_requestvote_resp_t msg;
  if (!read_value(ptr, remaining, msg.prevote))
    return "REQUEST_VOTE_RSP[malformed]";
  if (!skip_bytes(ptr, remaining, 4)) // padding after prevote
    return "REQUEST_VOTE_RSP[malformed]";
  if (!read_value(ptr, remaining, msg.request_term))
    return "REQUEST_VOTE_RSP[malformed]";
  if (!read_value(ptr, remaining, msg.term))
    return "REQUEST_VOTE_RSP[malformed]";
  if (!read_value(ptr, remaining, msg.vote_granted))
    return "REQUEST_VOTE_RSP[malformed]";

  std::ostringstream oss;
  oss << "REQUEST_VOTE_RSP{term=" << msg.term
      << ", granted=" << (msg.vote_granted ? "Y" : "N");
  if (msg.prevote)
    oss << ", prevote=1";
  oss << "}";
  return oss.str();
}

std::string parse_appendentries(const void *buf, size_t len)
{
  const uint8_t *ptr = static_cast<const uint8_t *>(buf);
  size_t remaining = len;

  // C struct layout with padding: leader_id(4) + pad(4) + msg_id(8) + term(8) +
  // prev_log_idx(8) + prev_log_term(8) + leader_commit(8) + n_entries(8)
  raft_appendentries_req_t msg;
  if (!read_value(ptr, remaining, msg.leader_id))
    return "APPEND_ENTRIES[malformed]";
  if (!skip_bytes(ptr, remaining, 4)) // padding after leader_id
    return "APPEND_ENTRIES[malformed]";
  if (!read_value(ptr, remaining, msg.msg_id))
    return "APPEND_ENTRIES[malformed]";
  if (!read_value(ptr, remaining, msg.term))
    return "APPEND_ENTRIES[malformed]";
  if (!read_value(ptr, remaining, msg.prev_log_idx))
    return "APPEND_ENTRIES[malformed]";
  if (!read_value(ptr, remaining, msg.prev_log_term))
    return "APPEND_ENTRIES[malformed]";
  if (!read_value(ptr, remaining, msg.leader_commit))
    return "APPEND_ENTRIES[malformed]";
  if (!read_value(ptr, remaining, msg.n_entries))
    return "APPEND_ENTRIES[malformed]";

  std::ostringstream oss;
  oss << "APPEND_ENTRIES{term=" << msg.term << ", prev_idx=" << msg.prev_log_idx
      << ", prev_term=" << msg.prev_log_term << ", commit=" << msg.leader_commit
      << ", n_ent=" << msg.n_entries;

  // Parse entries if present
  if (msg.n_entries > 0 && remaining >= 56)
  {
    oss << ", entries=[";
    for (int i = 0; i < msg.n_entries && remaining >= 56; i++)
    {
      if (i > 0)
        oss << ", ";

      // Parse entry header (56 bytes)
      // term(8) + id(8) + session(8) + type(4) + refs(2) + padding(2) +
      // user_data(8) + free_func(8) + data_len(8)
      raft_term_t entry_term;
      raft_entry_id_t entry_id;
      int entry_type;
      raft_size_t data_len;

      const uint8_t *entry_ptr = ptr;
      memcpy(&entry_term, entry_ptr, sizeof(entry_term));
      entry_ptr += sizeof(entry_term);
      memcpy(&entry_id, entry_ptr, sizeof(entry_id));
      entry_ptr += sizeof(entry_id) + sizeof(raft_session_t) + sizeof(int) +
                   sizeof(unsigned short) + sizeof(void *) + sizeof(void *);
      memcpy(&data_len, entry_ptr, sizeof(data_len));

      // Calculate actual entry size in the buffer
      size_t entry_total_size = 56 + data_len;

      oss << "{idx=" << (msg.prev_log_idx + i + 1) << ", term=" << entry_term
          << ", id=" << entry_id << ", type=" << entry_type
          << ", len=" << data_len << "}";

      // Skip to next entry
      if (remaining < entry_total_size)
        break;
      skip_bytes(ptr, remaining, entry_total_size);
    }
    oss << "]";
  }

  oss << "}";
  return oss.str();
}

std::string parse_appendentries_response(const void *buf, size_t len)
{
  const uint8_t *ptr = static_cast<const uint8_t *>(buf);
  size_t remaining = len;

  // C struct layout with padding: msg_id(8) + term(8) + success(4) + pad(4) +
  // current_idx(8)
  raft_appendentries_resp_t msg;
  if (!read_value(ptr, remaining, msg.msg_id))
    return "APPEND_ENTRIES_RSP[malformed]";
  if (!read_value(ptr, remaining, msg.term))
    return "APPEND_ENTRIES_RSP[malformed]";
  if (!read_value(ptr, remaining, msg.success))
    return "APPEND_ENTRIES_RSP[malformed]";
  if (!skip_bytes(ptr, remaining, 4)) // padding after success
    return "APPEND_ENTRIES_RSP[malformed]";
  if (!read_value(ptr, remaining, msg.current_idx))
    return "APPEND_ENTRIES_RSP[malformed]";

  std::ostringstream oss;
  oss << "APPEND_ENTRIES_RSP{term=" << msg.term
      << ", success=" << (msg.success ? "Y" : "N")
      << ", cur_idx=" << msg.current_idx << "}";
  return oss.str();
}

std::string parse_snapshot(const void *buf, size_t len)
{
  const uint8_t *ptr = static_cast<const uint8_t *>(buf);
  size_t remaining = len;

  // C struct layout with padding: term(8) + leader_id(4) + pad(4) + msg_id(8) +
  // snapshot_index(8) + snapshot_term(8) + chunk(offset(8) + len(8) +
  // last_chunk(4))
  raft_term_t term;
  raft_node_id_t leader_id;
  unsigned long msg_id;
  raft_index_t snapshot_index;
  raft_term_t snapshot_term;

  if (!read_value(ptr, remaining, term))
    return "SNAPSHOT[malformed]";
  if (!read_value(ptr, remaining, leader_id))
    return "SNAPSHOT[malformed]";
  if (!skip_bytes(ptr, remaining, 4))
    return "SNAPSHOT[malformed]";
  if (!read_value(ptr, remaining, msg_id))
    return "SNAPSHOT[malformed]";
  if (!read_value(ptr, remaining, snapshot_index))
    return "SNAPSHOT[malformed]";
  if (!read_value(ptr, remaining, snapshot_term))
    return "SNAPSHOT[malformed]";

  // Parse chunk: offset(8) + len(8) + last_chunk(4)
  raft_size_t chunk_offset = 0;
  raft_size_t chunk_len = 0;
  int last_chunk = 0;

  if (remaining >= sizeof(raft_size_t))
  {
    memcpy(&chunk_offset, ptr, sizeof(chunk_offset));
    ptr += sizeof(chunk_offset);
    remaining -= sizeof(chunk_offset);
  }
  if (remaining >= sizeof(raft_size_t))
  {
    memcpy(&chunk_len, ptr, sizeof(chunk_len));
    ptr += sizeof(chunk_len);
    remaining -= sizeof(chunk_len);
  }
  if (remaining >= sizeof(int))
  {
    memcpy(&last_chunk, ptr, sizeof(last_chunk));
  }

  std::ostringstream oss;
  oss << "SNAPSHOT{term=" << term << ", leader=" << leader_id
      << ", snap_idx=" << snapshot_index << ", snap_term=" << snapshot_term
      << ", chunk=[offset=" << chunk_offset << ", len=" << chunk_len
      << ", last=" << (last_chunk ? "Y" : "N") << "]}";
  return oss.str();
}

std::string parse_raft_message(const void *buf, size_t len)
{
  if (len < 4)
  {
    return "RAFT[too_short]";
  }

  int msg_type;
  memcpy(&msg_type, buf, sizeof(int));

  const uint8_t *msg_start = static_cast<const uint8_t *>(buf) + 4;
  size_t msg_len = len - 4;

  switch (msg_type)
  {
    case RAFT_MSG_APPENDENTRIES:
      return parse_appendentries(msg_start, msg_len);
    case RAFT_MSG_APPENDENTRIES_RESPONSE:
      return parse_appendentries_response(msg_start, msg_len);
    case RAFT_MSG_REQUESTVOTE:
      return parse_requestvote(msg_start, msg_len);
    case RAFT_MSG_REQUESTVOTE_RESPONSE:
      return parse_requestvote_response(msg_start, msg_len);
    case RAFT_MSG_SNAPSHOT:
      return parse_snapshot(msg_start, msg_len);
    case RAFT_MSG_ENTRY:
    case RAFT_MSG_ENTRY_RESPONSE:
    case RAFT_MSG_SNAPSHOT_RESPONSE:
    default:
      std::ostringstream oss;
      oss << "RAFT{type=" << msg_type << ",len=" << len << "}";
      return oss.str();
  }
}

} // namespace raft
