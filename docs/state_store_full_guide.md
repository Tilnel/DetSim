# StateStore 全解文档

## 目录
1. [概述](#概述)
2. [架构设计](#架构设计)
3. [三级存储层次](#三级存储层次)
4. [公共接口](#公共接口)
5. [内部实现](#内部实现)
6. [同步机制](#同步机制)
7. [状态转换](#状态转换)
8. [性能统计](#性能统计)

---

## 概述

StateStore 是 detsim 的核心状态管理组件，提供透明、分层的进程状态存储。它管理从内存（L1/L2）到磁盘的三级缓存，支持异步持久化、预取和自动压缩。

### 核心职责
- **状态存储**: 保存进程内存状态（通过 `save()`）
- **状态加载**: 按 hash 检索状态（通过 `load()`）
- **自动压缩**: 使用 ZSTD 压缩 L2 和磁盘数据
- **异步持久化**: 后台线程将状态写入磁盘
- **智能预取**: 基于访问模式预加载即将需要的状态

---

## 架构设计

### 整体架构图

```
┌─────────────────────────────────────────────────────────────┐
│                        应用程序层                            │
│                   (save / load / exists)                    │
└────────────────────┬────────────────────────────────────────┘
                     │
        ┌────────────▼────────────┐
        │    StateStore 单例      │
        │   (线程安全接口层)      │
        └────────────┬────────────┘
                     │
    ┌────────────────┼────────────────┐
    │                │                │
    ▼                ▼                ▼
┌────────┐    ┌──────────┐    ┌──────────┐
│  L1    │    │    L2    │    │   Disk   │
│ Hot    │◄──►│  Warm    │    │ Storage  │
│ Cache  │    │  Cache   │    │          │
│ (RAW)  │    │(COMPRESS)│    │(COMPRESS)│
└────────┘    └──────────┘    └──────────┘
    │                │                │
    ▼                ▼                ▼
  快速访问       较高容量         持久存储
  ~512MB        ~2GB             无限制
```

### 核心组件

1. **L1 Hot Cache**: 存储未压缩的原始数据（RAW），提供最快的访问速度
2. **L2 Warm Cache**: 存储压缩数据（COMPRESSED），平衡速度和容量
3. **Disk Storage**: 持久化存储，使用 ZSTD 压缩
4. **Worker Threads**: 异步处理压缩、持久化和预取

---

## 三级存储层次

### L1 Hot Cache（热缓存）

**特点**:
- 存储格式: 原始未压缩数据 (`std::vector<uint8_t> raw_data`)
- 默认容量: 512MB
- 访问延迟: 最低（直接内存访问）
- 驱逐策略: LRU（Least Recently Used）

**数据结构**:
```cpp
std::unordered_map<hash_type, StateEntryPtr> hot_cache_;  // hash -> entry
std::list<hash_type> hot_lru_;                              // LRU 链表
std::unordered_map<hash_type, list_iterator> hot_lru_index_; // hash -> LRU位置
std::atomic<size_t> hot_memory_usage_;                      // 当前内存使用量
```

**触发驱逐的条件**:
```cpp
if (hot_memory_usage_ + new_size > hot_cache_size * 1.2) {
    // 触发异步驱逐，目标是释放至少 10% 的容量
    evict_l1_async(to_evict);
}
```

**关键行为**:
- 允许 20% 的溢出（burst 缓冲）
- 每次驱逐至少释放 10% 容量（批量驱逐优化）
- 驱逐时压缩数据并移至 L2

### L2 Warm Cache（温缓存）

**特点**:
- 存储格式: ZSTD 压缩数据 (`std::vector<uint8_t> compressed_data`)
- 默认容量: 2GB
- 访问延迟: 中等（需要解压缩）
- 驱逐策略: LRU

**数据结构**:
```cpp
std::unordered_map<hash_type, StateEntryPtr> warm_cache_;
std::list<hash_type> warm_lru_;
std::unordered_map<hash_type, list_iterator> warm_lru_index_;
std::atomic<size_t> warm_memory_usage_;
```

**触发驱逐的条件**:
```cpp
if (warm_memory_usage_ + new_size > warm_cache_size) {
    evict_l2_if_needed(required_bytes);
}
```

**关键行为**:
- 严格容量限制（无溢出缓冲）
- 驱逐时直接删除压缩数据
- 如果数据已在磁盘，状态变为 PERSISTED；否则状态不确定

### Disk Storage（磁盘存储）

**特点**:
- 存储格式: ZSTD 压缩 + 元数据头部
- 容量: 无限制（受文件系统限制）
- 访问延迟: 最高（磁盘 I/O）
- 持久性: 进程重启后仍然保留

**存储路径**:
- 普通模式: `memory/<hash>.mem.zstd`
- Packed 模式: `memory_packed/`（单文件多状态存储）

**元数据结构**:
```cpp
struct PackedDataHeader {
    uint32_t magic;           // 'DSTM'
    uint64_t hash;
    size_t compressed_size;
    size_t original_size;     // 解压后的原始大小
    uint32_t checksum;
    // ...
};
```

---

## 公共接口

### 核心 API

#### 1. `hash_type save(const void *data, size_t len)`

**功能**: 保存数据到 StateStore，返回数据的 hash

**执行流程**:
1. 计算数据的 XXHash64 hash
2. 去重检查（L1 → L2 → Disk）
3. 如果已存在，直接返回 hash
4. 如果不存在，插入 L1（RAW 状态）
5. 将 entry 加入 IO 队列，触发异步压缩和持久化
6. 返回 hash

**阻塞点**:
- 获取 `hot_mutex_` 检查 L1
- 获取 `warm_mutex_` 检查 L2（通过 `in_l2()`）
- IO 队列反压：如果 `io_queue_` 满，阻塞在 `io_queue_not_full_cv_`

**返回值**:
- 成功: 数据的 hash（非零）
- 失败: 0（data 为 NULL 或 len 为 0）

**线程安全**: ✅ 线程安全，内部使用多个 mutex 保护

#### 2. `ssize_t load(hash_type hash, std::vector<uint8_t> &out_data)`

**功能**: 加载指定 hash 的数据

**查找顺序**:
1. **L1**: 如果存在且 `raw_data` 非空，直接返回
2. **L2**: 如果存在，解压缩后返回，并插入 L1
3. **Disk**: 从磁盘读取，插入 L1，然后返回
4. **失败**: 返回 -1

**阻塞点**:
- L1/L2 查找需要获取对应的 mutex
- L2 命中需要解压缩（CPU 密集型）
- Disk 查找需要磁盘 I/O
- 解压缩后插入 L1 可能需要驱逐

**返回值**:
- 成功: 数据大小（字节）
- 失败: -1（未找到）

**线程安全**: ✅ 线程安全

#### 3. `std::shared_ptr<const std::vector<uint8_t>> load_shared(hash_type hash)`

**功能**: 零拷贝加载数据，返回指向 L1 数据的 shared_ptr

**优势**: 避免数据复制，适合只读访问

**行为**:
- L1 命中: 返回指向 `raw_data` 的 shared_ptr
- L1 未命中: 调用 `load()` 加载到 L1，然后返回 shared_ptr

**线程安全**: ✅ 线程安全

#### 4. `bool exists(hash_type hash)`

**功能**: 检查数据是否存在（L1、L2 或 Disk）

**返回值**: true 表示存在，false 表示不存在

**线程安全**: ✅ 线程安全

#### 5. `bool wait_persisted(hash_type hash, uint64_t timeout_ms = 0)`

**功能**: 等待数据持久化到磁盘

**阻塞点**:
- 如果数据正在持久化（PERSISTING），阻塞在 `entry->cv`
- 如果指定 timeout_ms，最多等待指定时间
- timeout_ms = 0 表示无限等待

**返回值**:
- true: 已成功持久化或已在磁盘
- false: 超时或 entry 不存在且不在磁盘

**线程安全**: ✅ 线程安全（使用 per-entry 的 mutex 和 cv）

#### 6. `void wait_for_completion()`

**功能**: 等待所有待处理的 IO 操作完成

**使用场景**:
- 优雅关闭前确保数据完整性
- 信号处理（SIGINT）时等待写入完成

**阻塞点**:
- 轮询 IO 队列直到为空
- 检查 L1 中 PERSISTING 状态的 entry
- 刷新 packed storage 索引

**线程安全**: ✅ 线程安全

### 队列管理 API

StateStore 维护一个内部队列用于预取规划：

```cpp
void queue_push_back(hash_type hash);     // 添加 hash 到队列
hash_type queue_front();                  // 查看队首（不移除）
void queue_pop_front();                   // 移除队首并触发预取
hash_type queue_try_pop_front();          // 原子性检查并弹出
bool queue_empty() const;                 // 队列是否为空
size_t queue_size() const;                // 队列大小
hash_type queue_peek(size_t offset) const; // 查看指定偏移位置
```

**自动预取机制**:
- `queue_pop_front()` 会自动触发预取窗口内的状态
- 预取窗口大小由 `config.prefetch_window` 控制（默认 100）

### 配置和控制 API

```cpp
void init(const Config &config);          // 使用指定配置初始化
void init();                              // 使用默认配置初始化
void shutdown();                          // 优雅关闭
void disable_prefetch();                  // 禁用预取
void enable_prefetch(int prefetch_window); // 启用预取
```

### 统计信息 API

```cpp
Stats get_stats() const;                  // 获取统计信息副本
void reset_stats();                       // 重置统计
void print_stats() const;                 // 打印统计到 UI

// 缓存使用情况
size_t get_l1_usage() const;              // L1 当前使用（字节）
size_t get_l2_usage() const;              // L2 当前使用（字节）
size_t get_l1_capacity() const;           // L1 容量（字节）
size_t get_l2_capacity() const;           // L2 容量（字节）
size_t get_l1_entry_count() const;        // L1 entry 数量
size_t get_l2_entry_count() const;        // L2 entry 数量
```

---

## 内部实现

### 核心数据结构

#### StateEntry

表示单个状态的 entry，统一用于 L1 和 L2：

```cpp
struct StateEntry {
    hash_type hash{0};                                    // 状态 hash
    std::vector<uint8_t> raw_data;                        // L1: 原始数据
    std::shared_ptr<std::vector<uint8_t>> raw_data_ptr;   // 零拷贝访问用
    std::vector<uint8_t> compressed_data;                 // L2: 压缩数据
    size_t original_size = 0;                             // 原始数据大小
    
    enum class State {
        RAW,           // 有未压缩数据（在 L1）
        COMPRESSED,    // 只有压缩数据（在 L2）
        PERSISTING,    // 正在写入磁盘
        PERSISTED,     // 已在磁盘
        LOADING        // 正在从磁盘加载
    };
    std::atomic<State> state{State::RAW};
    
    std::atomic<uint64_t> last_access{0};                 // LRU 用时间戳
    std::condition_variable cv;                           // 等待持久化用
    std::mutex mutex;                                     // per-entry 锁
};
```

### 内部辅助函数

#### 缓存操作

```cpp
// 插入数据到 L1（RAW 状态）
void insert_to_l1(hash_type hash, std::vector<uint8_t> &&raw_data);

// 插入压缩数据到 L2（COMPRESSED 状态）
void insert_to_l2(hash_type hash, std::vector<uint8_t> &&compressed_data, 
                  size_t original_size);

// 将 L2 数据提升到 L1（需要解压缩）
bool move_l2_to_l1(hash_type hash);

// 如果 L1 内存不足，触发驱逐
void evict_l1_if_needed(size_t required_bytes);

// 如果 L2 内存不足，触发驱逐
void evict_l2_if_needed(size_t required_bytes);

// 更新 L1/L2 的 LRU
void update_hot_lru(hash_type hash);
void update_warm_lru(hash_type hash);
```

#### 压缩/解压缩

```cpp
// ZSTD 压缩，使用线程本地缓存的 context
std::vector<uint8_t> compress(const std::vector<uint8_t> &data);

// ZSTD 解压缩
std::vector<uint8_t> decompress(const std::vector<uint8_t> &data, 
                                size_t original_size);
```

**优化**: 使用线程本地 `ZSTDContextCache` 避免重复分配 ZSTD context（约 128MB）

#### 磁盘操作

```cpp
// 写入压缩数据到磁盘
bool write_to_disk(hash_type hash, 
                   const std::vector<uint8_t> &compressed_data,
                   size_t original_size);

// 从磁盘读取数据（解压后）
bool read_from_disk(hash_type hash, std::vector<uint8_t> &data);

// 从磁盘读取压缩数据（不解压）
bool read_compressed_from_disk(hash_type hash, 
                               std::vector<uint8_t> &compressed_data);

// 检查磁盘上是否存在
bool exists_on_disk(hash_type hash);
```

### Worker 线程

#### 1. io_worker()

**职责**: 异步压缩和持久化

**工作流程**:
1. 等待 `io_cv_`（有新 entry 加入 `io_queue_`）
2. 从队列取出 entry
3. **加锁复制** `raw_data`（防止 `eviction_worker` 清空）
4. 解压缩副本
5. 存储压缩数据到 `entry->compressed_data`
6. 设置 `original_size`
7. 状态改为 COMPRESSED
8. 状态改为 PERSISTING
9. 调用 `write_to_disk()` 写入磁盘
10. 成功后状态改为 PERSISTED
11. 插入 L2（冗余备份）
12. 通知 `entry->cv`（唤醒等待者）

**关键优化**: 压缩在锁外进行，只锁住复制和状态更新

#### 2. eviction_worker()

**职责**: 处理 L1 驱逐队列

**工作流程**:
1. 等待 `eviction_cv_`（有新 entry 加入 `eviction_queue_`）
2. 从队列取出 entry
3. 如果 `compressed_data` 存在，插入 L2
4. 清空 `raw_data`
5. 更新状态

**注意**: 这个 worker 主要处理已被 `evict_l1_async` 压缩的数据

#### 3. prefetch_worker()

**职责**: 预取即将需要的状态

**工作流程**:
1. 等待 `prefetch_cv_`（有新任务）
2. 检查 hash 是否已在 L1/L2
3. 如果已存在，从 inflight 集合移除
4. 如果不存在，从磁盘读取压缩数据
5. 插入 L2
6. 从 inflight 集合移除

**触发时机**:
- `queue_pop_front()` 自动触发
- 手动调用 `prefetch()` 或 `prefetch_batch()`

---

## 同步机制

### Mutex 列表

| Mutex | 类型 | 保护的数据 | 用途 |
|-------|------|-----------|------|
| `hot_mutex_` | `recursive_mutex` | `hot_cache_`, `hot_lru_`, `hot_lru_index_`, `hot_memory_usage_` | L1 缓存 |
| `warm_mutex_` | `recursive_mutex` | `warm_cache_`, `warm_lru_`, `warm_lru_index_`, `warm_memory_usage_` | L2 缓存 |
| `io_mutex_` | `mutex` | `io_queue_` | IO 队列 |
| `eviction_mutex_` | `mutex` | `eviction_queue_` | 驱逐队列 |
| `prefetch_mutex_` | `mutex` | `prefetch_queue_` | 预取队列 |
| `inflight_mutex_` | `mutex` | `prefetch_inflight_` | 正在预取的 hash 集合 |
| `queue_mutex_` | `mutex` | `state_queue_` | 状态队列 |
| `stats_mutex_` | `mutex` | `stats_` | 统计信息 |
| `ctx_cache_mutex_` | `mutex` | `ctx_caches_` | ZSTD context 缓存 |
| `entry->mutex` | `mutex` | `raw_data`, `compressed_data`, `original_size`, `state` | Per-entry 数据 |

### Condition Variable 列表

| CV | 用途 | 等待者 | 通知者 |
|----|------|--------|--------|
| `io_cv_` | IO 队列有新 entry | `io_worker()` | `save()` |
| `io_queue_not_full_cv_` | IO 队列有空间 | `save()` | `io_worker()` |
| `eviction_cv_` | 驱逐队列有新 entry | `eviction_worker()` | `evict_l1_async()` |
| `prefetch_cv_` | 预取队列有新任务 | `prefetch_worker()` | `submit_prefetch_tasks()` |
| `inflight_cv_` | Prefetch 完成 | `prefetch()` | `prefetch_worker()` |
| `entry->cv` | Entry 状态变为 PERSISTED | `wait_persisted()` | `io_worker()` |

### 死锁避免策略

#### 1. L1 和 L2 不嵌套锁定

```cpp
// 错误示范（可能死锁）:
lock(hot_mutex_);
lock(warm_mutex_);  // 如果另一个线程顺序相反，死锁！

// 正确做法（move_l2_to_l1）:
// Phase 1: 只锁 warm_mutex_ 读取数据
{
    lock_guard<warm_mutex_> lock(warm_mutex_);
    // 读取 compressed_data
}
// Phase 2: 解压（无锁）
decompress(...);
// Phase 3: 只锁 hot_mutex_ 插入 L1
{
    lock_guard<hot_mutex_> lock(hot_mutex_);
    // 插入 L1
}
// Phase 4: 只锁 warm_mutex_ 更新 L2 状态
{
    lock_guard<warm_mutex_> lock(warm_mutex_);
    // 更新状态
}
```

#### 2. Per-entry 锁只保护单个 entry

```cpp
// 先获取全局锁定位 entry
StateEntryPtr entry;
{
    lock_guard<hot_mutex_> lock(hot_mutex_);
    entry = hot_cache_[hash];
}
// 然后只锁这个 entry
{
    lock_guard<mutex> lock(entry->mutex);
    // 操作 entry 数据
}
```

#### 3. 压缩/解压无锁执行

```cpp
// 先复制数据（加锁）
vector<uint8_t> raw_copy;
{
    lock_guard<mutex> lock(entry->mutex);
    raw_copy = entry->raw_data;  // 复制
}
// 然后解压（无锁）
vector<uint8_t> compressed = compress(raw_copy);
```

---

## 状态转换

### StateEntry::State 状态机

```
                    ┌─────────────────────────────────────┐
                    │                                     ▼
┌──────────┐   ┌────────┐   ┌──────────┐   ┌──────────┐  ┌──────────┐
│   NEW    │──►│  RAW   │──►│COMPRESSED│──►│PERSISTING│─►│ PERSISTED│
└──────────┘   └────────┘   └──────────┘   └──────────┘  └──────────┘
                    │                           ▲              ▲
                    │                           │              │
                    └───────────────────────────┴──────────────┘
                            (从磁盘加载时)
```

### 状态转换表

| 当前状态 | 触发条件 | 下一状态 | 操作 |
|---------|---------|---------|------|
| RAW | `save()` 创建 | RAW | 插入 L1 |
| RAW | `evict_l1_async()` 压缩 | COMPRESSED | 压缩并移至 L2 |
| RAW | `io_worker()` 压缩 | COMPRESSED | 压缩完成 |
| COMPRESSED | `io_worker()` 开始写入 | PERSISTING | 开始磁盘写入 |
| PERSISTING | `write_to_disk()` 成功 | PERSISTED | 写入完成 |
| PERSISTED | `move_l2_to_l1()` 加载 | RAW | 解压并移回 L1 |
| PERSISTED | `load()` 从磁盘加载 | RAW | 从磁盘读取并插入 L1 |
| LOADING | 加载完成 | RAW/COMPRESSED | 加载成功 |

### 数据生命周期示例

**场景: 保存一个新状态**

1. **T=0**: 调用 `save(data, len)`
   - Entry 创建，状态 = RAW
   - 插入 L1
   - 加入 IO 队列

2. **T=1**: `io_worker()` 处理
   - 复制 `raw_data`
   - 压缩副本
   - 存储压缩数据
   - 状态 = COMPRESSED
   - 状态 = PERSISTING
   - 写入磁盘
   - 状态 = PERSISTED
   - 插入 L2

3. **T=2**: L1 满，触发驱逐
   - `evict_l1_async()` 选择这个 entry
   - 压缩 `raw_data`
   - 插入 L2
   - 清空 `raw_data`
   - 状态 = COMPRESSED（在 L2）

4. **T=3**: 再次访问
   - `load(hash)`
   - L1 未命中
   - L2 命中
   - 解压
   - 插入 L1
   - 状态 = RAW（同时在 L1 和 L2）

---

## 性能统计

### Stats 结构

```cpp
struct Stats {
    // 加载统计
    uint64_t load_requests;    // 总 load() 调用次数
    uint64_t l1_hits;          // L1 命中次数
    uint64_t l2_hits;          // L2 命中次数
    uint64_t disk_reads;       // 磁盘读取次数
    
    // 保存统计
    uint64_t save_calls;       // 总 save() 调用次数
    uint64_t save_dedup_hot;   // L1 去重次数
    uint64_t save_dedup_warm;  // L2 去重次数
    uint64_t save_dedup_disk;  // 磁盘去重次数
    uint64_t save_new;         // 实际保存的新状态数
    
    // 驱逐统计
    uint64_t evict_calls;      // 驱逐调用次数
    uint64_t evict_bytes;      // 驱逐的总字节数
    uint64_t evict_time_us;    // 驱逐总耗时（微秒）
    
    // 保存耗时统计
    uint64_t save_time_us;     // 保存总耗时（微秒）
    uint64_t save_new_slow;    // >1ms 的慢保存次数
    
    // 计算方法
    double hit_rate() const;   // (l1_hits + l2_hits) / load_requests
    double dedup_rate() const; // 去重率
};
```

### 统计信息解读

**命中率**:
```
hit_rate = (l1_hits + l2_hits) / load_requests * 100%
```
- 理想值 > 95%
- L1 命中率应远高于 L2

**去重率**:
```
dedup_rate = (save_dedup_hot + save_dedup_warm + save_dedup_disk) / save_calls * 100%
```
- 高去重率表示状态重复度高
- DFS 模式通常有较高去重率

**驱逐统计**:
- `evict_bytes` / `evict_calls` = 平均每次驱逐大小
- `evict_time_us` / `evict_calls` = 平均驱逐耗时

---

## 文件位置

- **头文件**: `/home/kaguya/code/detsim/src/core/state_store.h`
- **实现**: `/home/kaguya/code/detsim/src/core/state_store.cpp`
- **Packed 存储**: `/home/kaguya/code/detsim/src/core/state_store_packed.h`, `state_store_packed.cpp`

---

## 总结

StateStore 是一个设计精巧的三级缓存系统：

1. **L1 (RAW)**: 速度最快，容量最小，自动驱逐到 L2
2. **L2 (COMPRESSED)**: 速度中等，容量较大，使用 ZSTD 压缩
3. **Disk (PERSISTED)**: 速度最慢，容量无限，持久化存储

关键设计决策：
- **异步持久化**: 不阻塞 `save()` 调用
- **零拷贝访问**: `load_shared()` 避免数据复制
- **智能预取**: 基于访问模式提前加载
- **细粒度锁**: 全局锁 + per-entry 锁，最大化并发
- **死锁避免**: 严格的锁顺序规则