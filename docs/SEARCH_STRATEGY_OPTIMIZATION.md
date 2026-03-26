# 搜索策略优化指南

## 当前瓶颈分析

**确认性能瓶颈**：内存快照保存/恢复
- 每次状态切换：save (~2ms) + restore (~2ms)
- 300Hz 检查点 ≈ 每秒 600ms 纯开销
- **结论**：必须从"减少搜索状态数"角度优化，而非"加速单个状态"

## 优化方向 1: 启发式导向搜索

### 核心思想
用优先级队列替代 FIFO/LIFO，优先探索"更可能发现错误"的状态。

### 启发式指标

#### 1.1 路径深度惩罚
```cpp
score = base_score / (depth^2)
```
- 倾向于浅层状态（更快发现错误）
- 避免在深层单一路径上浪费太多时间

#### 1.2  syscall 稀有度
```cpp
// 记录每个 syscall 被调用的次数
if (rare_syscalls.count(syscall_nr)) {
    score += 100;  // 稀有 syscall 加分
}
```
- 罕见代码路径更可能有 bug

#### 1.3 覆盖率增长
```cpp
// 基于代码覆盖率的增长给分
new_coverage = get_coverage() - parent_coverage;
score += new_coverage * 10;
```
- 优先探索带来新覆盖率的状态

#### 1.4 错误模式匹配
```cpp
// 接近错误条件时加分
if (is_near_error_condition(state)) {
    score += 500;
}
```
- 例如：raft 中 term 不一致、日志空洞等

### 实现建议

修改 `state_queue` 为优先级队列：
```cpp
struct PrioritizedState {
    hash_type hash;
    double priority;
    bool operator<(const PrioritizedState& other) const {
        return priority < other.priority;  // 大顶堆
    }
};

std::priority_queue<PrioritizedState> prioritized_queue;
```

## 优化方向 2: 智能剪枝

### 2.1 对称性约减（Symmetry Reduction）

Raft 示例：3 个节点的角色分配
- (Leader=0, Follower=1,2) 和 (Leader=1, Follower=0,2) 在行为上对称
- 只搜索其中一种配置

```cpp
// 规范化状态表示
hash_type canonical_hash = normalize_by_symmetry(state);
if (visited.canonical_hashes.count(canonical_hash)) {
    prune();  // 剪枝对称状态
}
```

### 2.2 偏序约减（Partial Order Reduction）

独立事件可以交换顺序而不影响结果：
- 节点 A 和节点 B 的独立本地计算
- 只探索一种交错顺序

```cpp
// 判断两个事件是否独立
if (are_independent(event_a, event_b)) {
    // 只探索 a->b，跳过 b->a
}
```

### 2.3 基于哈希的采样

当状态空间爆炸时，随机采样一定比例的状态：
```cpp
// 概率性采样
if (hash % 100 < sample_rate) {
    add_to_queue(state);  // 只添加 10% 的状态
}
```

### 2.4 边界剪枝

基于领域知识的剪枝：
```cpp
// Raft: 如果 term 差异过大，可能是无效探索
if (state.max_term - state.min_term > TERM_THRESHOLD) {
    prune();
}
```

## 优化方向 3: 混合搜索策略

### 3.1 快速浅层 + 深度验证

阶段 1: BFS 到深度 5-8（快速覆盖浅层状态）
阶段 2: 对可疑状态进行深度 DFS

```cpp
if (depth < SHALLOW_DEPTH) {
    mode = MODE_BFS;
} else if (is_suspicious(state)) {
    mode = MODE_DFS_DEEP;
} else {
    mode = MODE_RANDOM;
}
```

### 3.2 错误注入引导

主动创造边界条件：
```cpp
// 在特定点注入故障
if (should_inject_fault(state)) {
    inject_network_partition();
    inject_node_crash();
}
```

## 优化方向 4: 状态压缩与缓存

### 4.1 增量快照

只保存差异而非完整状态：
```cpp
// 基于父状态的增量保存
StateDelta delta = compute_delta(parent_state, current_state);
save_delta(delta);  // 通常比完整状态小 10-100x
```

### 4.2 状态指纹（Fingerprinting）

用概率数据结构判断状态是否已访问：
```cpp
// Bloom filter 快速判断
if (bloom_filter.may_contain(hash)) {
    // 可能已访问，用精确检查确认
    if (state_set.count(hash)) {
        return;  // 剪枝
    }
}
```

## 针对 Raft 的具体建议

### 高价值搜索目标
1. **Leader 选举边界**：网络分区下的多 Leader 场景
2. **日志复制空洞**：缺失的中间日志项
3. **Snapshot 边界**：压缩时的状态一致性
4. **成员变更**：添加/删除节点时的安全性

### 剪枝策略示例
```cpp
bool should_prune_raft_state(const sys_state& s) {
    // 剪枝：所有节点都已退出
    if (all_exited(s)) return true;
    
    // 剪枝：不可能的配置（如 3 个 Leader）
    if (leader_count(s) > 1 && !network_partitioned(s)) {
        return true;  // 无分区时不可能多 Leader
    }
    
    // 剪枝：过期的 term（已确认安全）
    if (s.term < min_interesting_term) return true;
    
    return false;
}
```

## 实施优先级

| 优化 | 实现难度 | 效果 | 推荐度 |
|------|---------|------|--------|
| 启发式优先级队列 | 中 | 高 | ⭐⭐⭐⭐⭐ |
| 对称性约减 | 高 | 高 | ⭐⭐⭐ |
| 增量快照 | 高 | 极高 | ⭐⭐⭐⭐ |
| 智能剪枝 | 低 | 中 | ⭐⭐⭐⭐⭐ |
| 混合搜索 | 低 | 中 | ⭐⭐⭐⭐ |

## 推荐首先实现

1. **启发式分数**（1-2 天工作量，效果显著）
2. **简单剪枝**（半天工作量，立即可用）
3. **混合搜索模式**（1 天工作量，灵活性好）

## 监控指标

优化后应监控：
- 剪枝率：`pruned / generated`
- 覆盖率增长率：`new_coverage / states_searched`
- 错误发现时间：`time_to_first_bug`
- 有效状态比例：`unique / searched`
