# DetSim 测试 Redis 服务器完整研究报告

## 目录
1. [Redis 服务器架构分析](#redis-服务器架构分析)
2. [Redis 历史 Bug 分析](#redis-历史-bug-分析)
3. [DetSim 适配 Redis 的方案](#detsim-适配-redis-的方案)
4. [具体 Bug 测试案例](#具体-bug-测试案例)
5. [从零开始测试 Redis 的操作指南](#从零开始测试-redis-的操作指南)

---

## Redis 服务器架构分析

### 1.1 核心架构组件

Redis 服务器采用**单线程事件驱动**架构，主要包含以下核心组件：

| 组件 | 文件 | 功能 |
|------|------|------|
| 事件循环 | `ae.c` | 处理文件事件和时间事件 |
| 网络层 | `networking.c` | 客户端连接管理、协议解析 |
| 命令处理 | `server.c` | 命令查找、执行、返回结果 |
| 持久化 | `rdb.c`, `aof.c` | RDB快照、AOF日志 |
| 复制 | `replication.c` | 主从复制、同步 |

### 1.2 关键系统调用分析

#### 网络相关系统调用

Redis 使用 epoll 实现高性能网络 I/O：

```c
// ae_epoll.c - epoll 相关调用
epoll_create()    // 创建 epoll 实例
epoll_ctl()       // 注册/修改事件监听
epoll_wait()      // 等待事件触发（核心）

// 套接字相关
socket()          // 创建套接字
bind()            // 绑定地址
listen()          // 监听连接
accept()          // 接受连接
connect()         // 连接（从节点使用）
recv()/send()     // 数据收发
```

**epoll_wait 是 Redis 事件循环的核心**，每次调用都可能返回不同的事件集合，这是主要的非确定性来源。

#### 文件系统相关系统调用

```c
// RDB/AOF 持久化
open()            // 打开文件
read()/write()    // 读写数据
close()           // 关闭文件
fsync()/fdatasync()  // 强制同步到磁盘（AOF关键）
lseek()           // 文件定位
```

AOF 持久化使用 `fdatasync` 确保数据写入磁盘，这是性能瓶颈和非确定性来源。

#### 内存管理相关系统调用

```c
malloc()/free()   // 内存分配（使用 jemalloc）
mmap()/munmap()   // 内存映射
brk()             // 堆内存管理
```

#### 进程管理相关系统调用

```c
fork()            // 创建子进程（BGSAVE/BGREWRITEAOF）
```

Redis 使用 fork() 创建子进程进行后台持久化操作。

#### 时间相关系统调用

```c
gettimeofday()    // 获取当前时间（定时器、超时）
clock_gettime()   // 单调时钟
time()            // 秒级时间戳
```

`gettimeofday` 用于定时器管理、客户端超时检测、性能统计等。

### 1.3 非确定性来源分析

| 来源 | 系统调用 | 影响 |
|------|----------|------|
| **事件到达顺序** | epoll_wait | 客户端请求处理顺序不确定 |
| **持久化时机** | fsync/fdatasync | AOF 同步时间不确定 |
| **时间获取** | gettimeofday | 定时器触发时间不确定 |
| **fork 行为** | fork | 子进程创建时间不确定 |
| **网络延迟** | recv/send | 数据传输时间不确定 |

---

## Redis 历史 Bug 分析

### 2.1 CVE 漏洞（2020-2023）

| CVE | 描述 | 影响版本 | 修复版本 | 类型 |
|-----|------|----------|----------|------|
| **CVE-2020-14147** | Lua 整数溢出 | < 6.0.3 | 6.0.3+ | 内存损坏 |
| **CVE-2022-24736** | Lua NULL 指针解引用 | < 6.2.7, < 7.0.0 | 6.2.7, 7.0.0 | 崩溃 |
| **CVE-2022-31144** | XAUTOCLAIM 堆溢出 | < 7.0.4 | 7.0.4, 6.2.7 | DoS/RCE |
| **CVE-2022-36021** | SCAN/KEYS 拒绝服务 | < 6.0.18 | 6.0.18, 6.2.11, 7.0.9 | DoS |
| **CVE-2023-28425** | MSETNX 断言崩溃 | < 7.0.10 | 7.0.10+ | 崩溃 |
| **CVE-2023-28856** | HINCRBYFLOAT 哈希崩溃 | < 7.0.11 | 7.0.11, 6.2.12 | 崩溃 |
| **CVE-2023-22458** | HRANDFIELD/ZRANDMEMBER 崩溃 | < 6.2.9 | 6.2.9, 7.0.8 | 崩溃 |

### 2.2 可复现的 GitHub Issues

#### Issue #14838: prefetchCommands() 崩溃

- **描述**: 主从复制期间，加载 RediSearch 模块时崩溃
- **复现步骤**:
  1. 部署 Redis 8.4.0
  2. 开启主从复制
  3. 加载 RediSearch 模块
  4. 在同步期间执行 FT.SEARCH
- **崩溃点**: `prefetchCommands+0x495`
- **证据**: https://github.com/redis/redis/issues/14838

#### Issue #13649: BGSAVE 内存结构损坏

- **描述**: BGSAVE 期间内存分配异常导致崩溃
- **复现条件**: 大数据集（40+ GB），BGSAVE 过程中 OOM
- **证据**: https://github.com/redis/redis/issues/13649

#### Issue #13368: 3 节点复制崩溃

- **描述**: 3 节点复制环境中崩溃
- **证据**: https://github.com/redis/redis/issues/13368

### 2.3 适合 DetSim 测试的 Bug 类型

| Bug 类型 | 是否适合 | 原因 |
|----------|----------|------|
| **时序相关 Bug** | ✅ 高度适合 | 可通过控制 epoll_wait 复现 |
| **并发竞争 Bug** | ✅ 适合 | 可控制事件顺序 |
| **持久化 Bug** | ✅ 适合 | 可控制 fsync 时机 |
| **网络相关 Bug** | ✅ 适合 | 可模拟网络延迟/丢包 |
| **内存损坏 Bug** | ⚠️ 部分适合 | 需要配合内存检测工具 |

---

## DetSim 适配 Redis 的方案

### 3.1 已具备的能力

| 能力 | 实现位置 | 状态 |
|------|----------|------|
| 时间控制 | `emu_gettimeofday` | ✅ 已支持 |
| 网络基础 | `sockstate.cpp` | ✅ 已支持 UDP |
| 文件系统 | `fsstate.cpp` | ✅ 已支持基础 VFS |
| 状态保存 | `StateStore` | ✅ 已支持 |
| 插件系统 | `choose.cpp` | ✅ 已支持 |

### 3.2 需要扩展的部分

#### 1. epoll_wait 拦截（高优先级）

Redis 使用 epoll 作为事件驱动核心，必须实现 `choose_epoll_wait`：

```cpp
// 需要新增的插件函数
choose_out *choose_epoll_wait(int pid, int choice, const choose_in &in) {
    // 控制 epoll_wait 返回的事件数量和顺序
    // 这是控制 Redis 事件处理的关键
}
```

**实现要点**:
- 拦截 `epoll_wait` 系统调用
- 根据 `choice` 参数决定返回哪些事件
- 维护一个事件队列，按指定顺序返回

#### 2. fsync/fdatasync 拦截（高优先级）

AOF 持久化依赖 fsync，需要实现控制：

```cpp
choose_out *choose_fsync(int pid, int choice, const choose_in &in) {
    // 控制 fsync 返回时机
    // 可模拟磁盘延迟或失败
}
```

#### 3. accept 完整控制（中优先级）

当前 detsim 对 accept 的支持不完整，需要增强：

```cpp
choose_out *choose_accept(int pid, int choice, const choose_in &in) {
    // 控制客户端连接接受时机
    // 可模拟连接队列满的情况
}
```

#### 4. TCP 连接状态管理（中优先级）

当前 `sockstate` 主要针对 UDP，需要扩展 TCP 支持：

- TCP 三次握手模拟
- TCP 连接状态跟踪
- TCP 数据包重排序

### 3.3 插件开发计划

#### Phase 1: 基础系统调用支持

```cpp
// redis_choose.cpp - 第一阶段
extern "C" {
  // 时间控制（已支持）
  choose_out *choose_gettimeofday(int pid, int choice, const choose_in &in);
  
  // 新增：epoll_wait 控制
  choose_out *choose_epoll_wait(int pid, int choice, const choose_in &in);
  
  // 新增：fsync 控制
  choose_out *choose_fsync(int pid, int choice, const choose_in &in);
}
```

#### Phase 2: 网络事件控制

```cpp
// 新增：accept 控制
choose_out *choose_accept(int pid, int choice, const choose_in &in);

// 新增：recv/send 控制（增强版）
choose_out *choose_recv(int pid, int choice, const choose_in &in);
choose_out *choose_send(int pid, int choice, const choose_in &in);
```

#### Phase 3: Redis 协议解析插件

```cpp
// redis_parser.cpp
// 解析 Redis 协议（RESP），用于：
// 1. 调试输出
// 2. 命令级断言检查
// 3. 结果验证

std::string parse_redis_command(const void *buf, size_t len);
std::string parse_redis_reply(const void *buf, size_t len);
```

### 3.4 测试配置示例

```json
{
  "Loglevel": 2,
  "Nodes": 1,
  "Tracee": [
    ["/usr/bin/redis-server", "--port", "6379", "--appendonly", "yes"]
  ],
  "Addr": ["127.0.0.1"],
  "FSMap": [
    {"Host": "./redis_data", "Target": "/var/lib/redis"}
  ],
  "ChoosePoint": [
    {"syscall": "epoll_wait", "choose": 3},
    {"syscall": "gettimeofday", "choose": 2},
    {"syscall": "fsync", "choose": 2}
  ],
  "ChooseFunc": "./plugins/redis_choose.so",
  "UserCheck": "./plugins/redis_check.so",
  "StateStore": {
    "hot_cache_mb": 512,
    "warm_cache_mb": 2048
  }
}
```

---

## 具体 Bug 测试案例

### 4.1 案例：AOF 持久化时序 Bug

#### Bug 描述

在高并发写入场景下，AOF 的 `fdatasync` 调用可能在某些写操作完成前返回，导致数据丢失。

#### 复现步骤

1. 启动 Redis 启用 AOF (`appendonly yes`)
2. 设置 `appendfsync everysec`
3. 执行大量写操作
4. 在 fdatasync 调用时控制返回时机
5. 检查 AOF 文件完整性

#### DetSim 测试配置

```json
{
  "ChoosePoint": [
    {"syscall": "fdatasync", "choose": 2}
  ]
}
```

#### 检查插件

```cpp
// redis_check.cpp
int check_aof_consistency() {
    // 检查 AOF 文件内容是否完整
    // 验证命令序列是否正确
}
```

### 4.2 案例：客户端超时竞争条件

#### Bug 描述

客户端在命令执行过程中超时，可能导致命令已执行但客户端未收到响应，造成重复执行。

#### 复现步骤

1. 客户端连接 Redis
2. 发送耗时命令
3. 控制 epoll_wait 延迟返回
4. 模拟客户端超时
5. 检查命令执行次数

#### 测试方案

```cpp
choose_out *choose_epoll_wait(int pid, int choice, const choose_in &in) {
    struct epoll_event *events = (struct epoll_event *)malloc(...);
    
    if (choice == 0) {
        // 正常返回
        // ...
    } else {
        // 延迟返回（模拟超时）
        // 不返回任何事件，让命令继续执行
    }
    
    choose_out *out = new choose_out();
    out->args[0] = events;
    out->len[0] = num_events * sizeof(struct epoll_event);
    out->rval = num_events;
    return out;
}
```

---

## 从零开始测试 Redis 的操作指南

### 5.1 环境准备

```bash
# 1. 编译 Redis
wget https://download.redis.io/redis-7.0.0.tar.gz
tar xzf redis-7.0.0.tar.gz
cd redis-7.0.0
make

# 2. 确认系统调用支持
cd /home/kaguya/code/detsim
cat nr2call.c | grep -E "(epoll|fsync|gettimeofday)"
```

### 5.2 编写 Redis 专用插件

```cpp
// plugins/redis_choose.cpp
#include "emu.h"
#include "guest.h"
#include <sys/epoll.h>
#include <sys/time.h>

extern "C" {
  // 控制 epoll_wait 返回
  choose_out *choose_epoll_wait(int pid, int choice, const choose_in &in) {
    static int call_count = 0;
    call_count++;
    
    // 分配返回事件数组
    struct epoll_event *events = (struct epoll_event *)malloc(
        sizeof(struct epoll_event) * 10);
    
    int num_events = 0;
    
    if (choice == 0) {
      // 正常返回所有就绪事件
      // 从被测进程读取实际就绪的 fd
      // ...
      num_events = 1;  // 简化示例
    } else if (choice == 1) {
      // 只返回部分事件（模拟事件丢失）
      num_events = 0;
    } else {
      // 延迟返回（模拟超时）
      // 返回空事件，让调用超时
      num_events = 0;
    }
    
    choose_out *out = new choose_out();
    out->args[0] = events;
    out->len[0] = num_events * sizeof(struct epoll_event);
    out->rval = num_events;
    return out;
  }
  
  // 控制 fsync 行为
  choose_out *choose_fsync(int pid, int choice, const choose_in &in) {
    if (choice == 0) {
      // 正常返回
      choose_out *out = new choose_out();
      out->rval = 0;  // 成功
      return out;
    } else {
      // 模拟磁盘延迟或失败
      choose_out *out = new choose_out();
      out->rval = -1;  // 失败
      // 设置 errno
      return out;
    }
  }
}
```

### 5.3 编译插件

```bash
# DetSim 会自动编译 .cpp 为 .so
# 或手动编译：
g++ -shared -fPIC -o redis_choose.so redis_choose.cpp \
    -I/home/kaguya/code/detsim/src/subsys \
    -I/home/kaguya/code/detsim/src/core
```

### 5.4 编写测试配置

```json
{
  "Loglevel": 3,
  "Nodes": 1,
  "Tracee": [
    ["/usr/local/bin/redis-server", "/etc/redis/redis.conf"]
  ],
  "Addr": ["127.0.0.1"],
  "WorkingDir": "/",
  "FSMap": [
    {"Host": "./test_data", "Target": "/data"}
  ],
  "ChoosePoint": [
    {"syscall": "epoll_wait", "choose": 3},
    {"syscall": "gettimeofday", "choose": 2},
    {"syscall": "fsync", "choose": 2}
  ],
  "ChooseFunc": "./plugins/redis_choose.so",
  "UserCheck": "./plugins/redis_check.so",
  "StateStore": {
    "hot_cache_mb": 256,
    "warm_cache_mb": 1024,
    "enable_malloc_trim": true
  }
}
```

### 5.5 运行测试

```bash
# 1. 准备数据目录
mkdir -p test_data

# 2. 创建 Redis 配置
cat > test_data/redis.conf << EOF
port 6379
daemonize no
appendonly yes
appendfsync everysec
dir /data
EOF

# 3. 运行 DetSim
./tracer -c redis_test.json -i

# 4. 在交互模式中
# - 使用 'step' 单步执行
# - 使用 'choice 0' 或 'choice 1' 选择不同分支
# - 使用 'show' 查看当前状态
```

### 5.6 自动化测试脚本

```python
#!/usr/bin/env python3
# test_redis.py - 自动化 Redis 测试

import subprocess
import sys

def run_test(config_file, choices):
    """运行指定选择的测试"""
    cmd = ['./tracer', '-c', config_file]
    
    # 自动化交互
    process = subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    # 发送选择序列
    for choice in choices:
        process.stdin.write(f'choice {choice}\n')
        process.stdin.write('step\n')
    
    process.stdin.write('run\n')
    process.stdin.flush()
    
    stdout, stderr = process.communicate()
    return stdout, stderr

def main():
    # 测试不同的事件顺序
    test_cases = [
        ('正常顺序', [0, 0, 0]),
        ('延迟epoll', [1, 0, 0]),
        ('fsync失败', [0, 1, 0]),
    ]
    
    for name, choices in test_cases:
        print(f'\n=== 测试: {name} ===')
        stdout, stderr = run_test('redis_test.json', choices)
        
        # 检查结果
        if 'CHECK FAILED' in stdout:
            print(f'FAILED: 发现不一致')
        else:
            print(f'PASSED')

if __name__ == '__main__':
    main()
```

---

## 总结

### DetSim 测试 Redis 的关键点

| 方面 | 现状 | 需要的工作 |
|------|------|-----------|
| **事件循环** | 部分支持 | 完整实现 epoll_wait 控制 |
| **持久化** | 基础支持 | 增强 fsync/fdatasync 控制 |
| **网络** | UDP 支持 | 扩展 TCP 连接管理 |
| **时间** | 已支持 | 无需改动 |
| **验证** | 基础 | 开发 Redis 专用检查插件 |

### 推荐的测试优先级

1. **高优先级**: epoll_wait 控制系统调用
2. **高优先级**: AOF 持久化时序测试
3. **中优先级**: 客户端连接/超时测试
4. **中优先级**: 复制延迟测试
5. **低优先级**: 内存分配测试

### 预期发现的 Bug 类型

- 时序相关的竞争条件
- 持久化不完整问题
- 网络延迟导致的一致性问题
- 超时处理错误

---

## 参考资源

- Redis 官方文档: https://redis.io/documentation
- Redis GitHub: https://github.com/redis/redis
- DetSim 源码: `/home/kaguya/code/detsim/src/`
- 本报告生成的操作手册: `/home/kaguya/code/detsim/OPERATION_MANUAL.md`
