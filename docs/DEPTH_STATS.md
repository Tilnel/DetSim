# Depth Statistics Tracking

## 功能说明

Tracer 现在会自动记录搜索深度与状态数的关系，用于后续分析状态空间的增长趋势。

## 输出文件

文件：`depth_stats.txt`

格式：
```
# depth total_states_searched
1 150
2 892
3 4250
4 18234
...
```

- 第一列：`depth` - 当前搜索深度（从初始状态到当前状态的路径长度）
- 第二列：`total_states_searched` - 到该深度时的总搜索状态数（非 unique）

## 实现细节

### 触发条件
- 当 status monitor 线程检测到新的最大深度时触发
- 使用原子变量 `g_max_depth_recorded` 避免重复记录
- 每次只记录比上次更大的深度

### 代码位置
- 全局变量：`scheduler.cpp` - `g_max_depth_recorded`, `g_depth_stat_file`
- 记录逻辑：`status_monitor_thread()` 中深度计算之后
- 文件管理：`start_status_monitor()` 打开，`stop_status_monitor()` 关闭

## 使用示例

### 运行 tracer
```bash
cd examples/raft
./tracer -c raft.json
```

### 查看实时数据
```bash
# 另一个终端
tail -f depth_stats.txt
```

### 数据分析（Python）
```python
import matplotlib.pyplot as plt
import numpy as np

# 读取数据
data = np.loadtxt('depth_stats.txt', comments='#')
depths = data[:, 0]
states = data[:, 1]

# 绘制深度-状态数关系
plt.figure(figsize=(10, 6))
plt.plot(depths, states, 'b-o', markersize=3)
plt.xlabel('Search Depth')
plt.ylabel('Total States Searched')
plt.title('State Space Growth vs Depth')
plt.grid(True)
plt.savefig('depth_analysis.png')

# 计算每层的状态数
layer_states = np.diff(states, prepend=0)
plt.figure(figsize=(10, 6))
plt.bar(depths, layer_states)
plt.xlabel('Depth Layer')
plt.ylabel('New States in Layer')
plt.title('States per Depth Layer')
plt.savefig('layer_analysis.png')
```

## 应用场景

1. **状态空间估计**：通过拟合曲线预测总状态数
2. **分支因子分析**：计算平均分支因子 `states[i+1]/states[i]`
3. **搜索策略评估**：比较 BFS/DFS 的深度-状态分布
4. **复杂度验证**：验证实际复杂度与理论预期的偏差

## 注意事项

- 文件以 **append** 模式打开，多次运行会累积数据
- 建议在每次运行前手动删除或重命名旧文件
- 深度计算基于 `state_tree` 回溯，有一定计算开销
