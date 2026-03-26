# DetSim 测试 Redis/Raft 完整操作手册

## 目录
1. [概述](#概述)
2. [redisraft 示例深度解析](#redisraft-示例深度解析)
3. [DetSim 框架机制详解](#detsim-框架机制详解)
4. [测试 Redis 本体的方法](#测试-redis-本体的方法)
5. [具体 Bug 测试案例：Stale Snapshot Term](#具体-bug-测试案例-stale-snapshot-term)
6. [从零开始编写测试](#从零开始编写测试)

---

## 概述

本手册面向对 DetSim 和 Redis 都不熟悉的用户，通过一步步指导，让您能够使用 DetSim 测试 Raft 分布式一致性算法，并复现特定的分布式 Bug。

### 已具备的基础

在 `examples/redisraft` 目录下，已经有一个完整的 Raft 测试示例，它演示了：
- 如何使用 DetSim 测试 3 节点 Raft 集群
- 如何通过插件控制时间（超时）
- 如何验证 Raft 安全属性

---

## redisraft 示例深度解析

### 目录结构

```
examples/redisraft/
├── raft.json                  # 主配置文件
├── raft_check.json           # 带检查的配置
├── raft_stale_snapshot.json  # 测试 stale snapshot bug
├── raft2.json               # 2节点配置
├── raft_minimal.json        # 最小配置
├── plugins/                 # 插件目录
│   ├── choose.cpp          # 超时选择插件（源码）
│   ├── choose.so           # 编译后的插件
│   ├── check.cpp           # 属性检查插件（源码）
│   ├── check.so            # 编译后的插件
│   ├── raft_msg_parser.cpp # 消息解析插件
│   └── raft_msg_parser.so
└── raft/                   # 被测 Raft 程序
    ├── main.c              # 主程序源码
    ├── libraft.a           # Raft 库
    ├── include/            # 头文件
    │   ├── raft.h          # Raft API
    │   └── raft_private.h  # 内部结构
    └── tests/              # 原始测试
```

### 配置文件详解

#### raft.json 完整解析

```json
{
  "Loglevel": 2,                    # 日志级别：0=ERROR, 1=WARN, 2=INFO, 3=DEBUG
  "Nodes": 3,                       # 节点数量，必须和编译时的 NP 一致
  "Tracee": [                       # 被测程序配置
    ["./raft/raft", "0"],          # [可执行文件, 节点ID]
    ["./raft/raft", "1"],
    ["./raft/raft", "2"]
  ],
  "Addr": [                         # 虚拟网络地址
    "192.168.0.1",
    "192.168.0.2",
    "192.168.0.3"
  ],
  "WorkingDir": "/",               # 工作目录
  "SharedFiles": [],               # 共享文件列表（用于多进程共享内存）
  "ChoosePoint": [                  # 选择点配置
    { "syscall": 96, "choose": 2 }  # syscall 96 = gettimeofday，提供2个选择
  ],
  "ChooseFunc": "./plugins/choose.so",  # 选择函数插件路径
  "UserCheck": "./plugins/check.so",    # 用户检查插件路径
  "StateStore": {                   # 状态存储配置
    "hot_cache_mb": 512,           # 热缓存大小
    "warm_cache_mb": 2048,         # 温缓存大小
    "prefetch_window": 100,        # 预取窗口
    "compression_level": 1,        # 压缩级别
    "enable_malloc_trim": true,    # 启用内存整理
    "malloc_trim_threshold_mb": 64 # 内存整理阈值
  }
}
```

#### 关键字段说明

**ChoosePoint 选择点机制**
- `syscall`: 要拦截的系统调用号（96 是 gettimeofday）
- `choose`: 提供的非确定性选择数量
- 在 raft 示例中，这用于控制选举超时和请求超时

**Tracee 被测程序**
- 第一个参数是可执行文件路径（相对于配置文件目录）
- 后续参数传递给被测程序
- 这里传递节点 ID (0, 1, 2)

### 插件系统详解

#### 1. choose.cpp - 超时选择插件

**功能**：控制 Raft 的超时时间，实现确定性执行

```cpp
// 关键代码解析
extern "C"
{
  // 函数签名必须匹配：choose_out* func(int pid, int choice, const choose_in& in)
  choose_out *choose_gettimeofday(int pid, int choice, const choose_in &in)
  {
    // 1. 读取被测进程的 raft 结构体地址
    long praft = tracee_read_word(pid, (void *)get_var_addr("raft"));
    
    // 2. 读取 raft_server 结构体内容
    struct raft_server raft;
    tracee_read_mem(pid, (void *)praft, &raft, sizeof(raft));
    
    // 3. 根据节点角色（Leader/Follower）选择不同超时
    // RAFT_STATE_LEADER = 4
    if (raft.state == 4) {
      // Leader：使用 request_timeout
      if (choice == 0)
        *tv_out = tv;  // 不超时
      else
        *tv_out = tv_addmsec(tv, raft.request_timeout + 1);  // 超时
    } else {
      // Follower：使用 election_timeout_rand
      if (choice == 0)
        *tv_out = tv;  // 不超时
      else
        *tv_out = tv_addmsec(tv, raft.election_timeout_rand + 1);  // 超时触发选举
    }
    
    // 4. 构造返回值
    choose_out *out = new choose_out();
    out->args[0] = tv_out;
    out->len[0] = sizeof(struct timeval);
    out->rval = 0;
    return out;
  }
}
```

**工作原理**：
1. DetSim 在每次 gettimeofday 被调用时，会调用 `choose_gettimeofday`
2. 根据当前 `choice` 参数（0 或 1），决定是让时间正常流逝还是跳过超时
3. 这样可以控制 Raft 的选举和心跳超时，探索不同的执行路径

#### 2. check.cpp - 属性检查插件

**功能**：验证 Raft 分布式一致性算法的安全属性

```cpp
// 核心检查函数
extern "C" {
  int check() {
    int result = 0;
    
    // 对每个节点进行检查
    for (int i = 0; i < NP; i++) {
      result |= check_term_monotonicity(i);        // 检查 term 单调递增
      result |= check_commit_idx_monotonicity(i);  // 检查 commit_idx 单调递增
      result |= check_vote_integrity(i);           // 检查投票完整性
      result |= check_match_idx_monotonicity(i);   // 检查 match_idx 单调递增
    }
    
    // 全局检查
    result |= check_leader_uniqueness();           // 检查同一 term 只有一个 leader
    result |= check_leader_completeness();         // 检查 Leader Completeness 属性
    result |= check_snapshot_term_consistency();   // 检查 snapshot term 一致性
    result |= check_next_match_idx_relationship(); // 检查 next_idx > match_idx
    result |= check_applied_vs_committed();        // 检查 applied <= committed
    result |= check_committed_entries_replicated_to_majority(); // 检查多数派复制
    
    save_raft_state_to_ptmc();  // 保存当前状态供下次检查使用
    return result;  // 返回 0 表示所有检查通过
  }
}
```

**检查属性详解**：

1. **Term Monotonicity**：节点的 current_term 只能增加，不能减少
2. **Leader Uniqueness**：同一个 term 中只能有一个 leader
3. **Leader Completeness**：如果一个 entry 被提交，那么后续 term 的 leader 必须包含这个 entry
4. **Snapshot Term Consistency**：snapshot 的 term 不能大于当前 term

#### 3. raft_msg_parser.cpp - 消息解析插件

**功能**：解析 Raft 网络消息，用于调试输出

支持的消息类型：
- `APPEND_ENTRIES`：日志复制请求
- `APPEND_ENTRIES_RSP`：日志复制响应
- `REQUEST_VOTE`：投票请求
- `REQUEST_VOTE_RSP`：投票响应
- `SNAPSHOT`：快照传输

### 被测程序 main.c 分析

#### 核心数据结构

```c
#define NP 3
raft_server_t *raft;              // Raft 服务器实例
int self;                          // 当前节点ID
raft_node_t *nodes[NP];           // 其他节点指针
struct sockaddr_in addrs[NP];     // 节点网络地址
int sockfd;                        // UDP socket
```

#### 关键回调函数

Raft 库是纯算法实现，需要用户实现以下回调：

```c
// 1. 发送 RequestVote RPC
int send_requestvote(raft_server_t *raft, void *user_data, 
                     raft_node_t *node, raft_requestvote_req_t *msg);

// 2. 发送 AppendEntries RPC
int send_appendentries(raft_server_t *raft, void *user_data,
                       raft_node_t *node, raft_appendentries_req_t *msg);

// 3. 发送 Snapshot RPC
int send_snapshot(raft_server_t *raft, void *user_data,
                  raft_node_t *node, raft_snapshot_req_t *msg);

// 4. 应用日志到状态机
int applylog(raft_server_t *raft, void *user_data, 
             raft_entry_t *entry, raft_index_t entry_idx);

// 5. 持久化元数据
int persist_metadata(raft_server_t *raft, void *user_data,
                     raft_term_t term, raft_node_id_t vote);

// 6. 获取当前时间戳
raft_time_t timestamp(raft_server_t *raft, void *user_data);
```

#### 快照相关回调（redisraft 特有）

```c
// 加载快照
int load_snapshot(raft_server_t *raft, void *user_data,
                  raft_term_t snapshot_term, raft_index_t snapshot_index);

// 获取快照数据块
int get_snapshot_chunk(raft_server_t *raft, void *user_data, raft_node_t *node,
                       raft_size_t offset, raft_snapshot_chunk_t *chunk);

// 存储快照数据块
int store_snapshot_chunk(raft_server_t *raft, void *user_data,
                         raft_index_t snapshot_index, raft_size_t offset,
                         raft_snapshot_chunk_t *chunk);

// 清除快照
int clear_snapshot(raft_server_t *raft, void *user_data);
```

---

## DetSim 框架机制详解

### 插件接口定义

```cpp
// emu.h - 插件接口头文件
typedef struct choose_in {
  struct timeval now;
  choose_in(struct timeval tv) { now = tv; }
} choose_in;

typedef struct choose_out {
  void *args[6];    // 修改后的参数
  int len[6];       // 每个参数的长度（0表示不修改）
  int rval;         // 返回值
  
  ~choose_out() {   // 自动释放内存
    for (int i = 0; i < 6; i++) {
      if (len[i]) free(args[i]);
    }
  }
} choose_out;

// 选择函数类型定义
typedef choose_out *(*choose_func)(int pid, int choice, const choose_in &in);
```

### 编写自定义插件的步骤

#### 步骤 1：创建插件源文件

```cpp
// my_plugin.cpp
#include "emu.h"          // DetSim 插件接口
#include "guest.h"        // 访问被测进程内存的 API
#include "common.h"       // 公共定义
#include <sys/time.h>

extern "C" {
  // 实现选择函数
  choose_out *choose_gettimeofday(int pid, int choice, const choose_in &in) {
    struct timeval *tv_out = (struct timeval *)malloc(sizeof(*tv_out));
    
    if (choice == 0) {
      // 选择 0：使用原始时间
      *tv_out = in.now;
    } else {
      // 选择 1：让时间前进 100ms
      *tv_out = in.now;
      tv_out->tv_usec += 100000;
      if (tv_out->tv_usec >= 1000000) {
        tv_out->tv_sec += 1;
        tv_out->tv_usec -= 1000000;
      }
    }
    
    choose_out *out = new choose_out();
    out->args[0] = tv_out;
    out->len[0] = sizeof(struct timeval);
    out->rval = 0;  // 返回 0 表示成功
    return out;
  }
}
```

#### 步骤 2：编译插件

DetSim 会自动编译 `.cpp` 为 `.so`，也可以手动编译：

```bash
g++ -shared -fPIC -o my_plugin.so my_plugin.cpp \
    -I/path/to/detsim/src/subsys \
    -I/path/to/detsim/src/core
```

#### 步骤 3：在配置中引用

```json
{
  "ChoosePoint": [
    {"syscall": "gettimeofday", "choose": 2}
  ],
  "ChooseFunc": "./plugins/my_plugin.so"
}
```

### 关键 API 函数

#### 读取被测进程内存

```cpp
// 读取单个字（4字节或8字节，取决于架构）
long tracee_read_word(int pid, void *addr);

// 读取内存块
void tracee_read_mem(int pid, void *addr, void *buf, size_t len);

// 获取全局变量地址
void *get_var_addr(const char *var_name);
```

#### 表达式求值（用于复杂数据结构）

```cpp
#include "utils/expr_eval.hpp"

// 求值 C 表达式
long eval_long(const char *expr, int node_id, bool *success);
void *eval_ptr(const char *expr, int node_id, bool *success);

// 示例：读取 raft 结构体中的字段
std::string expr = "((raft_server*)raft)->current_term";
long term = eval_long(expr.c_str(), node_id, &success);
```

---

## 测试 Redis 本体的方法

### 与测试 Raft 的核心区别

| 方面 | Raft 示例 | Redis 本体 |
|------|-----------|------------|
| 网络协议 | 自定义 UDP (Raft RPC) | TCP (Redis Protocol) |
| 事件循环 | poll() | epoll() |
| 持久化 | 简单文件读写 | RDB/AOF 复杂格式 |
| 数据结构 | 简单日志 | 复杂数据类型 |
| 客户端交互 | 无 | 多客户端并发 |

### 测试 Redis 需要的额外工作

#### 1. 系统调用支持扩展

```cpp
// 需要新增的系统调用拦截

// epoll 相关
choose_out *choose_epoll_wait(int pid, int choice, const choose_in &in) {
  // 控制 epoll_wait 返回的事件数量和顺序
  // 这是控制 Redis 事件处理的关键
}

// 文件相关
choose_out *choose_open(int pid, int choice, const choose_in &in) {
  // 控制 RDB/AOF 文件的打开行为
}

choose_out *choose_fsync(int pid, int choice, const choose_in &in) {
  // 控制持久化时机，模拟磁盘延迟
}
```

#### 2. Redis 协议解析插件

```cpp
// redis_parser.cpp
// 解析 Redis 协议（RESP），用于调试输出和检查

std::string parse_redis_command(const void *buf, size_t len) {
  // 解析 *3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n
  // 返回可读格式：SET key value
}
```

#### 3. 测试配置示例

```json
{
  "Loglevel": 2,
  "Nodes": 1,                    // 先测试单节点
  "Tracee": [
    ["/usr/bin/redis-server", "--port", "6379", "--appendonly", "yes"]
  ],
  "Addr": ["127.0.0.1"],
  "FSMap": [                     // 文件系统映射
    {"Host": "./redis_data", "Target": "/var/lib/redis"}
  ],
  "ChoosePoint": [
    {"syscall": "epoll_wait", "choose": 3},
    {"syscall": "fsync", "choose": 2},
    {"syscall": "gettimeofday", "choose": 2}
  ],
  "ChooseFunc": "./plugins/redis_choose.so",
  "UserCheck": "./plugins/redis_check.so"
}
```

---

## 具体 Bug 测试案例：Stale Snapshot Term

### Bug 描述

**问题**：当 Leader 创建 snapshot 后 term 增加，然后某个节点重新加载了旧的 snapshot，导致 term 回退（term regression），违反了 Raft 的安全性。

**影响**：可能导致同一 term 有两个 leader，或者已提交的 entry 丢失。

### 测试配置

已提供的配置：`raft_stale_snapshot.json`

```json
{
  "Loglevel": 2,
  "Nodes": 3,
  "Tracee": [
    ["./raft/raft", "0"],
    ["./raft/raft", "1"],
    ["./raft/raft", "2"]
  ],
  "Addr": ["192.168.0.1", "192.168.0.2", "192.168.0.3"],
  "UserCheck": "./plugins/check.so",
  "ChooseFunc": "./plugins/choose.so",
  "CheckInterval": 1,           // 每步都检查
  "ChoosePoint": [
    {"syscall": "gettimeofday", "choose": 2}
  ],
  "Description": "Test for stale snapshot term bug. Leader creates snapshot, term increases, then node reloads stale snapshot causing term regression."
}
```

### 触发 Bug 的步骤

#### 步骤 1：编译和准备

```bash
# 1. 进入目录
cd /home/kaguya/code/detsim/examples/redisraft

# 2. 编译被测程序
cd raft
make clean
make

# 3. 返回上级目录
cd ..

# 4. 确认插件已编译（如果没有，DetSim 会自动编译）
ls plugins/*.so
```

#### 步骤 2：运行测试

```bash
# 使用 detsim 运行测试（假设 detsim 已编译）
./tracer -c raft_stale_snapshot.json

# 或者在交互模式下运行
./tracer -c raft_stale_snapshot.json -i
```

#### 步骤 3：在交互模式中探索

启动后会进入 DetSim 交互界面，常用命令：

```
# 基本命令
help          # 显示帮助
show          # 显示当前状态
step          # 执行一步
run           # 自动运行直到完成或失败
reset         # 重置到初始状态

# 状态查看
show syscall  # 显示当前系统调用
show history  # 显示执行历史
show queue    # 显示待执行的状态队列

# 选择控制
choice 0      # 选择分支 0（不超时）
choice 1      # 选择分支 1（超时）
```

### 如何确认发现了 Bug

当 `check.so` 检测到属性违反时，会输出类似：

```
[CHECK FAILED] Node 1 loaded stale snapshot!
  snapshot_term=2, current_term=3
```

或

```
[CHECK FAILED] Node 0 term decreased: 3 -> 2
  This violates Raft's term monotonicity property!
```

### 复现 Bug 的具体场景

要手动触发这个 bug，需要按以下时序：

1. **节点 0 成为 Leader**（term=1）
2. **节点 0 创建 snapshot**（snapshot_term=1）
3. **节点 0 增加 term**（term=2，可能因为网络分区后恢复）
4. **节点 1 崩溃并重启**
5. **节点 1 从 Leader 接收 snapshot**（snapshot_term=1）
6. **节点 1 加载 snapshot，term 回退到 1** ❌ **BUG!**

通过控制 `choice` 参数，可以精确控制超时和网络时序，重现这个场景。

---

## 从零开始编写测试

### 完整示例：编写一个简单的 Leader Election 测试

#### 步骤 1：创建被测程序

```c
// simple_raft.c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>

// 简化的 Raft 状态
int node_id;
int current_term = 0;
int voted_for = -1;
enum { STATE_FOLLOWER, STATE_CANDIDATE, STATE_LEADER } state = STATE_FOLLOWER;

struct timeval last_heartbeat;
int election_timeout;  // 随机选举超时（ms）

// 获取当前时间（毫秒）
long long get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// 处理选举超时
void on_election_timeout() {
    if (state == STATE_LEADER) return;
    
    // 转换为 candidate
    state = STATE_CANDIDATE;
    current_term++;
    voted_for = node_id;
    
    printf("Node %d: Become CANDIDATE, term=%d\n", node_id, current_term);
    
    // 请求投票（简化：直接假设获得多数票）
    // 实际应该发送 RequestVote RPC
    
    // 模拟成为 Leader
    if (node_id == 0) {  // 简化：节点 0 总是获胜
        state = STATE_LEADER;
        printf("Node %d: Become LEADER, term=%d\n", node_id, current_term);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <node_id>\n", argv[0]);
        return 1;
    }
    
    node_id = atoi(argv[1]);
    election_timeout = 150 + (node_id * 50);  // 不同的超时时间
    
    gettimeofday(&last_heartbeat, NULL);
    
    printf("Node %d: Start as FOLLOWER, election_timeout=%d\n", 
           node_id, election_timeout);
    
    // 主循环
    while (1) {
        long long now = get_time_ms();
        long long last = last_heartbeat.tv_sec * 1000 + 
                        last_heartbeat.tv_usec / 1000;
        
        // 检查选举超时
        if (state != STATE_LEADER && (now - last) > election_timeout) {
            on_election_timeout();
            gettimeofday(&last_heartbeat, NULL);
        }
        
        usleep(10000);  // 10ms
    }
    
    return 0;
}
```

#### 步骤 2：编译被测程序

```bash
gcc -o simple_raft simple_raft.c
```

#### 步骤 3：编写插件

```cpp
// simple_choose.cpp
#include "emu.h"
#include "guest.h"
#include <sys/time.h>
#include <cstdlib>

extern "C" {
  choose_out *choose_gettimeofday(int pid, int choice, const choose_in &in) {
    struct timeval *tv_out = (struct timeval *)malloc(sizeof(*tv_out));
    
    if (choice == 0) {
      // 选择 0：时间正常流逝（不触发超时）
      *tv_out = in.now;
    } else {
      // 选择 1：让时间前进 200ms（触发超时）
      *tv_out = in.now;
      tv_out->tv_usec += 200000;
      if (tv_out->tv_usec >= 1000000) {
        tv_out->tv_sec += 1;
        tv_out->tv_usec -= 1000000;
      }
    }
    
    choose_out *out = new choose_out();
    out->args[0] = tv_out;
    out->len[0] = sizeof(struct timeval);
    out->rval = 0;
    return out;
  }
}
```

#### 步骤 4：编写检查插件

```cpp
// simple_check.cpp
#include "common.h"
#include "monitor.h"
#include "utils/expr_eval.hpp"
#include <fmt/format.h>

extern "C" {
  int check() {
    // 检查 term 单调递增
    for (int i = 0; i < NP; i++) {
      std::string expr = fmt::format("current_term");
      bool success = false;
      long term = eval_long(expr.c_str(), i, &success);
      
      if (success) {
        static long prev_terms[10] = {0};
        if (term < prev_terms[i]) {
          detsim::ui::ui_printf(
            "[CHECK FAILED] Node %d term decreased: %ld -> %ld\n",
            i, prev_terms[i], term);
          return 1;
        }
        prev_terms[i] = term;
      }
    }
    
    // 检查只有一个 Leader  per term
    int leader_count = 0;
    for (int i = 0; i < NP; i++) {
      std::string expr = "state";
      bool success = false;
      long s = eval_long(expr.c_str(), i, &success);
      if (success && s == 2) {  // STATE_LEADER = 2
        leader_count++;
      }
    }
    
    if (leader_count > 1) {
      detsim::ui::ui_printf(
        "[CHECK FAILED] Multiple leaders: %d\n", leader_count);
      return 1;
    }
    
    return 0;
  }
}
```

#### 步骤 5：编写配置

```json
{
  "Loglevel": 2,
  "Nodes": 3,
  "Tracee": [
    ["./simple_raft", "0"],
    ["./simple_raft", "1"],
    ["./simple_raft", "2"]
  ],
  "Addr": ["192.168.0.1", "192.168.0.2", "192.168.0.3"],
  "ChoosePoint": [
    {"syscall": "gettimeofday", "choose": 2}
  ],
  "ChooseFunc": "./simple_choose.so",
  "UserCheck": "./simple_check.so"
}
```

#### 步骤 6：运行测试

```bash
./tracer -c simple_config.json -i
```

---

## 调试技巧

### 1. 查看被测进程的内部状态

```cpp
// 在 check 函数中打印状态
int check() {
    for (int i = 0; i < NP; i++) {
        std::string term_expr = "((raft_server*)raft)->current_term";
        std::string state_expr = "((raft_server*)raft)->state";
        
        bool success;
        long term = eval_long(term_expr.c_str(), i, &success);
        long state = eval_long(state_expr.c_str(), i, &success);
        
        const char* state_str = 
            state == 1 ? "FOLLOWER" :
            state == 2 ? "CANDIDATE" :
            state == 4 ? "LEADER" : "UNKNOWN";
        
        detsim::ui::ui_printf(
            "Node %d: term=%ld, state=%s\n", i, term, state_str);
    }
    return 0;
}
```

### 2. 打印系统调用信息

DetSim 会自动打印被拦截的系统调用。可以在配置中设置：

```json
{
  "Loglevel": 3  // DEBUG 级别会显示所有系统调用
}
```

### 3. 使用 GDB 调试被测进程

```bash
# 在另一个终端启动 gdb
gdb -p $(pgrep -f "raft/raft")

# 设置断点
break raft_recv_appendentries
break raft_recv_requestvote

# 继续执行
continue
```

---

## 常见问题

### Q: 插件编译失败怎么办？

A: 检查以下几点：
1. 确保头文件路径正确（`emu.h`, `guest.h` 等）
2. 使用 `-shared -fPIC` 编译选项
3. 确保函数使用 `extern "C"` 导出

### Q: 如何选择正确的系统调用号？

A: 在 x86_64 Linux 上：
```bash
# 查看系统调用号
cat /usr/include/asm-generic/unistd.h | grep gettimeofday
# 输出：#define __NR_gettimeofday 96
```

或使用：
```cpp
#include <sys/syscall.h>
int syscall_num = SYS_gettimeofday;  // 自动获取正确的号码
```

### Q: 如何知道被测进程的全局变量地址？

A: 使用 `get_var_addr` 函数：
```cpp
void *raft_addr = get_var_addr("raft");  // 获取全局变量 "raft" 的地址
```

### Q: 检查失败时如何定位问题？

A: 在交互模式中使用以下命令：
```
show history    # 查看执行历史
show state      # 查看当前状态
backtrack       # 回溯到上一步
```

---

## 总结

通过本手册，您应该能够：

1. **理解** DetSim 的工作原理和插件机制
2. **运行**现有的 redisraft 测试
3. **编写**自定义的测试插件
4. **复现**具体的 Raft 分布式 Bug
5. **扩展**框架以测试更复杂的系统（如 Redis）

关键要点：
- **ChoosePoint** 控制非确定性（时间、网络、IO）
- **Check Plugin** 验证安全属性
- **State Exploration** 探索所有可能的执行路径

---

## 参考资源

- DetSim 源码：`/home/kaguya/code/detsim/src/`
- Raft 论文：https://raft.github.io/raft.pdf
- redisraft 源码：`/home/kaguya/code/detsim/examples/redisraft/raft/`
- Jepsen 测试报告：https://jepsen.io/analyses/redis-raft-1b3fbf6
