#include "include/raft.h"
/* #include "include/raft_private.h" */
#include "debug.h"
#include <assert.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/poll.h>
#include <malloc.h>
#include <sys/time.h>
#include <errno.h>

#define NP 3
raft_server_t *raft;
int self;
raft_node_t nodes[NP];
const struct sockaddr_in addrs[NP] = {
  { .sin_family = AF_INET, .sin_addr.s_addr = 0x0100a8c0, .sin_port = 0x2823 },
  { .sin_family = AF_INET, .sin_addr.s_addr = 0x0200a8c0, .sin_port = 0x2823 },
  { .sin_family = AF_INET, .sin_addr.s_addr = 0x0300a8c0, .sin_port = 0x2823 }
};
int sockfd;
long msec_elapsed = 200;

/* Snapshot support */
#define SNAPSHOT_BUF_SIZE (32 * 1024)
static char snapshot_buf[SNAPSHOT_BUF_SIZE];
static int snapshot_len = 0;
static int snapshot_in_progress = 0;

/* Applied log index for snapshot tracking */
static raft_index_t applied_idx = 0;

/* Large message buffer for send/recv - must hold snapshot data */
#define MSG_BUF_SIZE (SNAPSHOT_BUF_SIZE + 256)
static char msg_buf[MSG_BUF_SIZE];
static char recv_buf[MSG_BUF_SIZE];

enum {
  RAFT_MSG_APPENDENTRIES,
  RAFT_MSG_APPENDENTRIES_RESPONSE,
  RAFT_MSG_REQUESTVOTE,
  RAFT_MSG_REQUESTVOTE_RESPONSE,
  RAFT_MSG_ENTRY,
  RAFT_MSG_ENTRY_RESPONSE,
  RAFT_MSG_SNAPSHOT,
  RAFT_MSG_SNAPSHOT_RESPONSE
};

struct sockaddr_in getdest(void *node) {
  struct sockaddr_in dest;
  int i;
  for (i = 0; i < NP; i++) {
    if (node == nodes[i]) 
    {
      dest = addrs[i];
      break;
    }
  }
  assert(i != NP);
  return dest;
}

void *getnode(struct sockaddr_in *addr) {
  void *node;
  int i;
  for (i = 0; i < NP; i++) {
    if (addr->sin_addr.s_addr == addrs[i].sin_addr.s_addr && addr->sin_port == addrs[i].sin_port)
    {
      node = nodes[i];
      break;
    }
  }
  if (i == NP) {
    for (int j = 0; j < sizeof(struct sockaddr_in); j++) {
    }
  }
  fsync(1);
  assert(i != NP);
  return node;
}

int send_requestvote(
    raft_server_t *raft, 
    void *user_data, 
    raft_node_t *node,
    msg_requestvote_t *msg) {

  struct sockaddr_in dest = getdest(node);
  int type = RAFT_MSG_REQUESTVOTE;
  size_t pos = 0;
  
  memcpy(msg_buf, &type, sizeof(int));
  pos += sizeof(int);
  memcpy(msg_buf + pos, msg, sizeof(*msg));
  pos += sizeof(*msg);
  memcpy(msg_buf + pos, &self, sizeof(self));
  pos += sizeof(self);

  sendto(sockfd, msg_buf, pos, 0, (struct sockaddr *)&dest, sizeof(dest));
  return 0;
}

int send_requestvote_response(
    raft_server_t *raft, 
    void *user_data, 
    raft_node_t *node,
    msg_requestvote_response_t *msg) {
  struct sockaddr_in dest = getdest(node);
  int pos = 0;
  int type = RAFT_MSG_REQUESTVOTE_RESPONSE;
  memcpy(msg_buf, &type, sizeof(int));
  pos += sizeof(int);
  memcpy(msg_buf + pos, msg, sizeof(*msg));
  pos += sizeof(*msg);
  memcpy(msg_buf + pos, &self, 4);
  pos += 4;
  sendto(sockfd, msg_buf, pos, 0, (struct sockaddr *)&dest, sizeof(dest));
  return 0;
}

int send_appendentries_response(
    raft_server_t *raft, 
    void *user_data, 
    raft_node_t *node,
    msg_appendentries_response_t *msg) {
  struct sockaddr_in dest = getdest(node);
  int pos = 0;
  int type = RAFT_MSG_APPENDENTRIES_RESPONSE;
  memcpy(msg_buf, &type, sizeof(int));
  pos += sizeof(int);
  memcpy(msg_buf + pos, msg, sizeof(*msg));
  pos += sizeof(*msg);
  sendto(sockfd, msg_buf, pos, 0, (struct sockaddr *)&dest, sizeof(dest));
  return 0;
}

int send_appendentries(
    raft_server_t *raft, 
    void *user_data, 
    raft_node_t *node,
    msg_appendentries_t *msg) {
  
  struct sockaddr_in dest = getdest(node);
  size_t pos = 0;
  int type = RAFT_MSG_APPENDENTRIES;
  
  memcpy(msg_buf, &type, sizeof(int));
  pos += sizeof(int);

  /* Calculate header size safely using char* arithmetic */
  size_t header_size = (char*)&msg->entries - (char*)msg;
  if (pos + header_size > MSG_BUF_SIZE) return -1;
  memcpy(msg_buf + pos, msg, header_size);
  pos += header_size;

  for (int i = 0; i < msg->n_entries; i++) 
  {
    raft_entry_t *entry = &msg->entries[i];
    raft_entry_data_t *data = &entry->data;

    /* Calculate entry header size */
    size_t entry_header = (char*)data - (char*)entry;
    if (pos + entry_header + sizeof(data->len) + data->len > MSG_BUF_SIZE) return -1;
    
    memcpy(msg_buf + pos, entry, entry_header);
    pos += entry_header;
    memcpy(msg_buf + pos, &data->len, sizeof(data->len));
    pos += sizeof(data->len);
    memcpy(msg_buf + pos, data->buf, data->len);
    pos += data->len;
  }
  sendto(sockfd, msg_buf, pos, 0, (struct sockaddr *)&dest, sizeof(dest));
  return 0;
}

/* Snapshot callback: Send snapshot to a node */
int send_snapshot(
    raft_server_t *raft,
    void *user_data,
    raft_node_t *node)
{
  struct sockaddr_in dest = getdest(node);
  size_t pos = 0;
  int type = RAFT_MSG_SNAPSHOT;
  
  memcpy(msg_buf, &type, sizeof(int));
  pos += sizeof(int);
  
  /* Include snapshot metadata */
  raft_index_t snap_idx = raft_get_snapshot_last_idx(raft);
  raft_term_t snap_term = raft_get_snapshot_last_term(raft);
  
  /* Check buffer bounds */
  if (pos + sizeof(snap_idx) + sizeof(snap_term) + sizeof(snapshot_len) > MSG_BUF_SIZE) {
    return -1;
  }
  
  memcpy(msg_buf + pos, &snap_idx, sizeof(snap_idx));
  pos += sizeof(snap_idx);
  memcpy(msg_buf + pos, &snap_term, sizeof(snap_term));
  pos += sizeof(snap_term);
  memcpy(msg_buf + pos, &snapshot_len, sizeof(snapshot_len));
  pos += sizeof(snapshot_len);
  
  if (snapshot_len > 0 && snapshot_len < SNAPSHOT_BUF_SIZE) {
    /* Check if snapshot fits in message buffer */
    if (pos + snapshot_len > MSG_BUF_SIZE) {
      return -1;
    }
    memcpy(msg_buf + pos, snapshot_buf, snapshot_len);
    pos += snapshot_len;
  }
  
  sendto(sockfd, msg_buf, pos, 0, (struct sockaddr *)&dest, sizeof(dest));
  return 0;
}

int applylog(
    raft_server_t *raft, 
    void *user_data, 
    raft_entry_t *entry,
    raft_index_t entry_idx) {
  /* Track applied index for snapshotting */
  applied_idx = entry_idx;
  return 0;
}

int persist_vote(
    raft_server_t *raft, 
    void *user_data, 
    raft_node_id_t vote) {
  return 0;
}

int persist_term(
    raft_server_t *raft, 
    void *user_data, 
    raft_term_t term,
    raft_node_id_t vote) {
  return 0;
}

int log_offer(
    raft_server_t *raft, 
    void *user_data, 
    raft_entry_t *entry,
    raft_index_t entry_idx) {
  return 0;
}

int log_pop(
    raft_server_t *raft, 
    void *user_data, 
    raft_entry_t *entry,
    raft_index_t entry_idx) {
  return 0;
}

__attribute__((noinline))
int poll_msg() {
  struct sockaddr_in sender_addr = { 0 };
  socklen_t addr_len = sizeof(sender_addr);
  int recvlen = 0;
  
  while ((recvlen = recvfrom(sockfd, recv_buf, MSG_BUF_SIZE, 0, (struct sockaddr *)&sender_addr, &addr_len)) > 0)
  {
    void *sender = getnode(&sender_addr);
    int type;
    size_t pos = 0;
    
    /* Check minimum message size */
    if (recvlen < sizeof(type)) continue;
    
    memcpy(&type, recv_buf + pos, sizeof(type));
    pos += sizeof(type);
    
    switch (type)
    {
    case RAFT_MSG_APPENDENTRIES:
    {
      /* Check minimum header size */
      if (pos + sizeof(msg_appendentries_t) > recvlen) break;
      
      msg_appendentries_t *ae = malloc(sizeof(*ae));
      if (!ae) break;
      
      /* Calculate header size safely */
      size_t header_size = (char*)&ae->entries - (char*)ae;
      if (pos + header_size > recvlen) { free(ae); break; }
      
      memcpy(ae, recv_buf + pos, header_size);
      pos += header_size;
      
      ae->entries = NULL;
      if (ae->n_entries > 0) {
        ae->entries = malloc(ae->n_entries * sizeof(msg_entry_t));
        if (!ae->entries) { free(ae); break; }
        
        for (int i = 0; i < ae->n_entries; i++) {
          raft_entry_t *entry = &ae->entries[i];
          
          /* Check bounds for entry header */
          size_t entry_header = (char*)&entry->data - (char*)entry;
          if (pos + entry_header + sizeof(int) > recvlen) {
            /* Clean up allocated memory */
            for (int j = 0; j < i; j++) free(ae->entries[j].data.buf);
            free(ae->entries);
            free(ae);
            ae = NULL;
            break;
          }
          
          memcpy(entry, recv_buf + pos, entry_header);
          pos += entry_header;
          memcpy(&entry->data.len, recv_buf + pos, sizeof(int));
          pos += sizeof(int);
          
          /* Check bounds for data */
          if (pos + entry->data.len > recvlen) {
            for (int j = 0; j < i; j++) free(ae->entries[j].data.buf);
            free(ae->entries);
            free(ae);
            ae = NULL;
            break;
          }
          
          entry->data.buf = malloc(entry->data.len);
          if (!entry->data.buf) {
            for (int j = 0; j < i; j++) free(ae->entries[j].data.buf);
            free(ae->entries);
            free(ae);
            ae = NULL;
            break;
          }
          memcpy(entry->data.buf, recv_buf + pos, entry->data.len);
          pos += entry->data.len;
        }
      }
      
      if (ae) {
        msg_appendentries_response_t response;
        raft_recv_appendentries(raft, sender, ae, &response);
        send_appendentries_response(raft, NULL, sender, &response);
        
        /* Free allocated memory */
        if (ae->entries) {
          for (int i = 0; i < ae->n_entries; i++) {
            free(ae->entries[i].data.buf);
          }
          free(ae->entries);
        }
        free(ae);
      }
    }
    break;
    case RAFT_MSG_APPENDENTRIES_RESPONSE:
    {
      if (pos + sizeof(msg_appendentries_response_t) > recvlen) break;
      msg_appendentries_response_t response;
      memcpy(&response, recv_buf + pos, sizeof(response));
      pos += sizeof(response);
      raft_recv_appendentries_response(raft, sender, &response);
      break;
    }
    case RAFT_MSG_REQUESTVOTE:
    {
      if (pos + sizeof(msg_requestvote_t) > recvlen) break;
      msg_requestvote_response_t response;
      msg_requestvote_t requestvote;
      memcpy(&requestvote, recv_buf + pos, sizeof(requestvote));
      pos += sizeof(requestvote);
      raft_recv_requestvote(raft, sender, &requestvote, &response);
      send_requestvote_response(raft, NULL, sender, &response);
      break;
    }
    case RAFT_MSG_REQUESTVOTE_RESPONSE: 
    {
      if (pos + sizeof(msg_requestvote_response_t) > recvlen) break;
      msg_requestvote_response_t response;
      memcpy(&response, recv_buf + pos, sizeof(response));
      pos += sizeof(response);
      raft_recv_requestvote_response(raft, sender, &response);
      break;
    }
    case RAFT_MSG_SNAPSHOT:
    {
      /* Receive snapshot from leader */
      if (pos + sizeof(raft_index_t) + sizeof(raft_term_t) + sizeof(int) > recvlen) break;
      
      raft_index_t snap_idx;
      raft_term_t snap_term;
      int snap_len;
      
      memcpy(&snap_idx, recv_buf + pos, sizeof(snap_idx));
      pos += sizeof(snap_idx);
      memcpy(&snap_term, recv_buf + pos, sizeof(snap_term));
      pos += sizeof(snap_term);
      memcpy(&snap_len, recv_buf + pos, sizeof(snap_len));
      pos += sizeof(snap_len);
      
      /* Validate snapshot length */
      if (snap_len < 0 || snap_len > SNAPSHOT_BUF_SIZE) break;
      if (pos + snap_len > recvlen) break;
      
      if (snap_len > 0) {
        memcpy(snapshot_buf, recv_buf + pos, snap_len);
        snapshot_len = snap_len;
        
        /* Load the snapshot */
        raft_begin_load_snapshot(raft, snap_term, snap_idx);
        
        /* Restore membership: re-add all peer nodes.
         * raft_begin_load_snapshot removes all nodes except self from the
         * nodes array. We need to re-add them to prevent single-node bootstrap.
         * Note: raft_add_node will check if node exists; since nodes were
         * removed from array but may still exist in memory, we simply add
         * them back. raft_add_node handles duplicates gracefully. */
        for (int i = 0; i < NP; i++) {
          if (i != self) {
            /* Active node back; it will be voting by default */
            nodes[i] = raft_add_node(raft, (void *)&addrs[i], i, 0);
            if (nodes[i] == NULL) {
              nodes[i] = raft_get_node(raft, i);
            }
            raft_node_set_active(nodes[i], 1);
          }
        }
        
        raft_end_load_snapshot(raft);
      }
      break;
    }
    }
    /* Note: pos may not equal recvlen if message has extra padding */
    (void)pos; /* Suppress unused warning if NDEBUG */
  }
  return 114514;
}

void logging(raft_server_t *s, raft_node_t *n, void *data, const char *buf) {
  puts(buf);
}

/* Use static buffer to avoid dangling pointer issue */
static char entry_data_storage[256];
static int entry_counter = 0;

void leader_generate_entries() {
  if (!raft_is_leader(raft)) {
    return;
  }
  
  /* Create a new log entry - use static buffer to avoid dangling pointer */
  snprintf(entry_data_storage, sizeof(entry_data_storage), 
           "entry_%d_from_node_%d", entry_counter, self);
  
  raft_entry_t *entry = (raft_entry_t *)malloc(sizeof(raft_entry_t));
  entry->data.buf = strdup(entry_data_storage);
  entry->data.len = strlen(entry_data_storage) + 1;
  entry->id = entry_counter++;
  
  msg_entry_response_t response;
  int e = raft_recv_entry(raft, entry, &response);
  
  if (e != 0) {
    /* Entry not accepted (maybe not leader anymore) */
  }
}

/* Trigger snapshot creation when log grows too large */
void maybe_create_snapshot() {
  raft_index_t commit_idx = raft_get_commit_idx(raft);
  raft_index_t snap_idx = raft_get_snapshot_last_idx(raft);
  
  /* Create snapshot when we have 2+ new committed entries */
  if (commit_idx - snap_idx >= 2 && !snapshot_in_progress) {
    int e = raft_begin_snapshot(raft, 0);
    if (e == 0) {
      snapshot_in_progress = 1;
      
      /* Generate some snapshot data */
      snprintf(snapshot_buf, SNAPSHOT_BUF_SIZE, 
               "snapshot_at_idx_%ld_term_%ld_node_%d",
               commit_idx, raft_get_current_term(raft), self);
      snapshot_len = strlen(snapshot_buf) + 1;
      
      raft_end_snapshot(raft);
      snapshot_in_progress = 0;
    }
  }
}

// Main function
int main(int argc, const char *argv[]) {
  self = atoi(argv[1]);

  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  Assert(bind(sockfd, (struct sockaddr *)&addrs[self], sizeof(struct sockaddr_in)) == 0, "node %s", argv[1]);
  
  // Initialize Raft server
  raft = raft_new();

  // Set callbacks
  raft_cbs_t callbacks = {
    .send_requestvote = send_requestvote,
    .send_appendentries = send_appendentries,
    .applylog = applylog,
    .persist_vote = persist_vote,
    .persist_term = persist_term,
    .log_offer = log_offer,
    .log_pop = log_pop,
    .send_snapshot = send_snapshot,
  };
  raft_set_callbacks(raft, &callbacks, NULL);
  raft_set_election_timeout(raft, 1000);

  // Add nodes
  nodes[0] = raft_add_node(raft, (void *)&addrs[0], 0, 0 == self);
  nodes[1] = raft_add_node(raft, (void *)&addrs[1], 1, 1 == self);
  nodes[2] = raft_add_node(raft, (void *)&addrs[2], 2, 2 == self);

  // if (self == 0) 
  // {
  //   raft_periodic(raft, 2000);
  // }

  struct timeval tv_old = {.tv_sec = 0, .tv_usec = 0};
  while (1)
  {
    struct timeval tv_new;
    gettimeofday(&tv_new, NULL);
    int elapsed = ((tv_new.tv_sec - tv_old.tv_sec) * 1000000 + (tv_new.tv_usec - tv_old.tv_usec)) / 1000;
    /* Leader generates entries periodically */
    leader_generate_entries();

    raft_periodic(raft, elapsed);
    tv_old = tv_new;
    
    /* Maybe create snapshot */
    maybe_create_snapshot();
    
    poll_msg();
  }

  raft_free(raft);

  return 0;
}
