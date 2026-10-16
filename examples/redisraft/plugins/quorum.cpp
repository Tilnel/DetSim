/* Fixed three-member, no-compaction replication scenario. */
#define REDISRAFT_PLUGIN
#define RAFT_REQUIRE_REPLICA_QUORUM
#define RAFT_SERVER_TYPE "raft_server"
#define RAFT_LOG_TYPE "raft_log_t"
#define RAFT_LEADER_ROLE 4
#define RAFT_PLUGIN_ID "redisraft.replication-quorum"
#include "../../raft/plugins/check_common.hpp"
