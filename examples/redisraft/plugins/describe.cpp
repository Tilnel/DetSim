/* Optional display plugin. This protocol knowledge is never linked into DetSim. */
#include "api/plugin.h"
#include "raft_msg_parser.h"
#include <cstring>
namespace {
int describe(detsim_bytes bytes, detsim_writer output) {
  try {
    if (bytes.size < sizeof(int)) return 0;
    int type;
    std::memcpy(&type, bytes.data, sizeof(type));
    if (type < raft::RAFT_MSG_APPENDENTRIES || type > raft::RAFT_MSG_SNAPSHOT_RESPONSE) return 0;
    auto text = raft::parse_raft_message(bytes.data, bytes.size);
    return output.append(output.context, text.data(), text.size()) ? -1 : 1;
  } catch (...) { return -1; }
}
const detsim_plugin_v1 descriptor{DETSIM_PLUGIN_ABI_V1, sizeof(detsim_plugin_v1), "redisraft.message-display", 0,
 nullptr, nullptr, nullptr, nullptr, nullptr, describe};
}
extern "C" const detsim_plugin_v1 *detsim_plugin_entry_v1() { return &descriptor; }
