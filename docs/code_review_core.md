# detsim core/ 目录代码审查报告

## 概述

本报告针对 detsim 项目 `src/core/` 目录下的代码进行架构审查，重点关注设计问题、模块边界、命名清晰度和错误处理等方面。共识别出 **50+ 处**需要改进的地方。

---

## 一、模块边界问题 (Module Boundary Issues)

### 1.1 scheduler.cpp 越界管理状态
**位置**: `scheduler.cpp:1193-1222`, `1369-1413`, `1539-1602`
**问题**: 三个探索模式（BFS/DFS/Rand）都直接调用 `s.recover_running_state()` 和 `dest_state.save_metadata()`，这些状态管理操作应该在 state 模块中封装。
**建议**: 创建 `StateTransition` 类，将状态迁移逻辑从 scheduler 移至 state.cpp。

### 1.2 state.cpp 反向依赖 ptmc_state
**位置**: `state.cpp:589-598`
**问题**: `tracee_state::recover_running_state()` 直接修改全局 `ptmc_state.sock_states/fs_states/raft_states`，造成循环依赖。
**建议**: 通过参数传递或回调机制解耦，让 state 模块不依赖全局状态。

### 1.3 scheduler 直接操作 StateStore 队列
**位置**: `scheduler.cpp:1138-1139`, `1517-1518`
**问题**: scheduler 直接调用 `StateStore::instance().queue_clear()` 和 `state_queue_append()`。
**建议**: 队列操作应该完全封装在 state 模块，scheduler 只调用高层次的 `enqueue_state()` 接口。

### 1.4 init_state() 跨越多个职责
**位置**: `scheduler.cpp:1647-1699`
**问题**: 函数同时处理进程 fork、信号设置、FdManager 初始化、首个状态记录。
**建议**: 拆分为 `fork_tracees()`, `init_subsystems()`, `record_initial_state()`。

### 1.5 check_state() 位置不当
**位置**: `scheduler.cpp:561-587`
**问题**: 状态检查逻辑在 scheduler 中，但操作的是状态数据。
**建议**: 移至 state.cpp 作为 `sys_state::validate()` 方法。

### 1.6 UI 与核心逻辑混合
**位置**: `scheduler.cpp:239-265`
**问题**: NCursesUI 提示逻辑直接嵌入 syscall 处理流程。
**建议**: 抽象 `IUserChoiceProvider` 接口，由上层注入实现。

### 1.7 state_to_be_discarded() 死代码
**位置**: `scheduler.cpp:1098-1114`
**问题**: 函数开头直接 `return 0`，后续代码永远不会执行。
**建议**: 修复逻辑或删除该函数。

---

## 二、代码重复问题 (Code Duplication)

### 2.1 状态恢复/保存模板重复
**位置**: 
- BFS: `scheduler.cpp:1193-1222`
- DFS: `scheduler.cpp:1369-1413`
- Rand: `scheduler.cpp:1539-1602`
**重复代码**: 
```cpp
s.recover_running_state();
// ... timing code ...
ptmc_state.dest_state = sys_state(syscall_info);
ptmc_state.dest_state.save_metadata();
// ... timing code ...
state_tree_add(...);
```
**建议**: 提取为 `exploration_step(source, syscall_info, dest)` 模板函数。

### 2.2 状态统计更新重复
**位置**: `scheduler.cpp:1267-1270`, `1599-1601`
**问题**: 相同的统计更新逻辑在多个地方复制。
**建议**: 封装为 `update_exploration_stats()` 函数。

### 2.3 SIGINT 处理重复
**位置**: `scheduler.cpp:1272-1296`, `1605-1630`
**问题**: 中断处理逻辑几乎完全相同。
**建议**: 提取为 `handle_interrupt()` 函数。

### 2.4 探索模式状态机重复
**位置**: `scheduler.cpp:1163-1173`
**问题**: 模式选择 switch 在多处出现。
**建议**: 使用 Strategy 模式封装三种探索策略。

### 2.5 起始/结束报告重复
**位置**: `scheduler.cpp:1300-1307`, `1634-1642`
**问题**: 结果输出逻辑重复。
**建议**: 提取为 `report_exploration_summary()`。

### 2.6 状态验证逻辑重复
**位置**: `scheduler.cpp:1225-1238`, `1587-1598`, `1424-1437`
**问题**: `check_state()` 调用和错误处理重复。
**建议**: 统一封装。

---

## 三、命名与可读性问题 (Naming & Readability)

### 3.1 模糊的类型别名
**位置**: `state.cpp:667-669`
**问题**: `LSS`, `SSS`, `TSS` 类型别名过于简短，含义不清。
**建议**: 重命名为 `StateQueue`, `StateHashSet`, `StateParentTree`。

### 3.2 不一致的命名风格
**位置**: `state.h`, `scheduler.cpp`
**问题**: 混用 snake_case (如 `syscall_info`) 和 camelCase (如 `tracee_state`)。
**建议**: 统一代码风格。

### 3.3 缩写不清晰
**位置**: 多处
**问题**: `ptmc` (是什么缩写？), `CKPT` (checkpoint?), `DISDEAD`。
**建议**: 添加注释说明或使用完整名称。

### 3.4 魔术数字
**位置**: `scheduler.cpp:1246`, `274`
**问题**: `114514` 等魔术数字没有说明。
**建议**: 定义为常量 `DUMMY_TID_VALUE`。

### 3.5 变量名与类型名冲突
**位置**: `scheduler.cpp:1324`
**问题**: `syscall_info syscall_info[NP]` 变量名与类型名相同。
**建议**: 改为 `syscall_infos` 或 `info_array`。

### 3.6 状态命名混淆
**问题**: `source_state`, `dest_state`, `current_state` 在不同上下文含义不同。
**建议**: 统一命名约定：
- `parent_state` / `child_state`（树结构）
- `before_state` / `after_state`（时间顺序）

### 3.7 函数命名不一致
**位置**: `scheduler.cpp`
**问题**: `exec_bfs()` vs `do_dfs()` vs `exec_rand()` - 前缀不统一。
**建议**: 统一为 `explore_bfs()`, `explore_dfs()`, `explore_rand()`。

---

## 四、全局状态与耦合问题 (Global State & Coupling)

### 4.1 过度使用的全局变量 ptmc_state
**位置**: `monitor.h:79`
**问题**: 整个核心模块依赖单一全局状态对象。
**建议**: 封装为 `SimulationContext` 类，通过依赖注入传递。

### 4.2 全局信号处理器依赖全局状态
**位置**: `scheduler.cpp:73-87`
**问题**: `exit_all()` 直接访问 `StateStore::instance()` 和全局状态。
**建议**: 使用更安全的信号处理模式（如 signalfd 或专用线程）。

### 4.3 extern 声明分散
**位置**: `scheduler.cpp:21,43,89,90,667-670`
**问题**: 多个 extern 声明散布在文件各处。
**建议**: 集中到一个头文件或封装为接口类。

### 4.4 静态全局变量
**位置**: `scheduler.cpp:694-703`, `1313-1314`
**问题**: `g_depth_stat_file`, `g_fit_a`, `states_searched_this_run` 等。
**建议**: 封装到 `ExplorationStatistics` 类。

### 4.5 模式判断通过全局枚举
**位置**: `monitor.h:29-34`
**问题**: `PTMC_STATE::mode` 是全局状态的一部分。
**建议**: 探索模式作为策略对象，而非全局状态。

### 4.6 状态队列全局可访问
**位置**: `state.cpp` 全局队列
**问题**: `state_queue`, `state_set`, `state_tree` 全局可访问。
**建议**: 封装到 `StateGraph` 类，提供受控访问接口。

---

## 五、错误处理问题 (Error Handling)

### 5.1 assert 用于运行时检查
**位置**: `scheduler.cpp:105,108,116,119,458`
**问题**: 使用 assert 检查 ptrace 结果，发布版本可能被禁用。
**建议**: 改为显式错误处理和返回码。

### 5.2 不一致的错误返回类型
**位置**: 多处
**问题**: 混用 `bool`, `int`, `void` 返回错误状态。
**建议**: 使用 `std::optional` 或自定义 `Result<T>` 类型。

### 5.3 静默失败
**位置**: `scheduler.cpp:219`
**问题**: `analyze_choose()` 默认返回 0，可能掩盖错误。
**建议**: 返回 `std::optional<int>` 或错误码。

### 5.4 LOG_CRIT 后继续执行
**位置**: `scheduler.cpp:1189-1191`, `1340-1345`
**问题**: 关键错误日志后继续执行，可能导致未定义行为。
**建议**: 关键错误后应该终止或回滚。

### 5.5 缺少资源清理
**位置**: `scheduler.cpp:1264-1265`
**问题**: `state_fetched->clear(); delete state_fetched;` 应该在异常安全上下文中执行。
**建议**: 使用智能指针 `std::unique_ptr<sys_state>`。

### 5.6 文件操作无错误检查
**位置**: `scheduler.cpp:1070-1071`
**问题**: `g_depth_stat_file` 可能为 nullptr 但直接使用。
**建议**: 添加空指针检查。

### 5.7 内存分配失败未处理
**位置**: `state_store.cpp` 多处
**问题**: `new` 操作可能抛出 `std::bad_alloc`。
**建议**: 捕获异常或检查分配结果。

---

## 六、设计模式问题 (Design Patterns)

### 6.1 缺乏策略模式
**问题**: DFS/BFS/Rand 通过 if/switch 分支实现。
**建议**: 
```cpp
class IExplorationStrategy {
    virtual void explore_next() = 0;
};
class BFSStrategy : public IExplorationStrategy { ... };
class DFSStrategy : public IExplorationStrategy { ... };
```

### 6.2 庞大的状态结构体
**位置**: `monitor.h:27-74`
**问题**: `PTMC_STATE` 包含 30+ 字段，混合了配置、运行时状态、统计信息。
**建议**: 拆分为：
- `Configuration`（配置）
- `RuntimeState`（运行时）
- `ExplorationStatistics`（统计）

### 6.3 缺少访问者模式
**问题**: 状态序列化逻辑分散在多处。
**建议**: 使用访问者模式统一处理。

### 6.4 紧耦合的 syscalls 处理
**位置**: `scheduler.cpp:151-189`
**问题**: `on_syscall_enter()` 是大 switch 语句。
**建议**: 使用命令模式或函数表分发。

### 6.5 缺少观察者模式
**问题**: 状态变更（如探索完成）直接调用 UI 更新。
**建议**: 使用事件/观察者模式解耦。

---

## 七、性能与资源管理问题 (Performance & Resources)

### 7.1 频繁的内存分配
**位置**: `scheduler.cpp:625`, `1265`
**问题**: `new sys_state()` 频繁分配。
**建议**: 使用对象池。

### 7.2 重复的文件系统操作
**位置**: `state.cpp:667`
**问题**: `/proc/{pid}/maps` 每次捕获都重新打开。
**建议**: 缓存或批量读取。

### 7.3 无限制的队列增长
**问题**: BFS 模式下 `state_queue` 可能无限增长。
**建议**: 添加背压机制或内存限制。

### 7.4 定时器精度问题
**位置**: `scheduler.cpp:985`
**问题**: `time(NULL)` 精度只有秒级。
**建议**: 使用 `std::chrono::steady_clock`。

---

## 八、测试与可维护性问题 (Testing & Maintainability)

### 8.1 函数过长
**位置**: `scheduler.cpp`
- `exec_bfs()`: ~200 行
- `exec_rand()`: ~150 行
- `do_dfs()`: ~150 行
**建议**: 提取辅助函数，单函数不超过 50 行。

### 8.2 缺少单元测试接口
**问题**: 核心逻辑与全局状态强耦合，无法单元测试。
**建议**: 引入依赖注入，提供 mock 接口。

### 8.3 代码注释不足
**问题**: 复杂算法（如指数拟合 `update_depth_fit`）缺少数学说明。
**建议**: 添加公式注释和参考文献。

### 8.4 版本控制污染
**位置**: `scheduler.cpp:71`
**问题**: 注释掉的代码 `// static pid_t pgid = 114514;`
**建议**: 删除或说明保留原因。

---

## 九、具体小错误 (Specific Bugs)

### 9.1 条件编译宏使用不当
**位置**: `scheduler.cpp:41`
**问题**: `#define pids ptmc_state.pids` 宏可能产生副作用。
**建议**: 使用内联函数替代。

### 9.2 潜在的整数溢出
**位置**: `scheduler.cpp:1322`
**问题**: `states_searched_this_run` 是 `int` 可能溢出。
**建议**: 使用 `size_t` 或 `uint64_t`。

### 9.3 竞态条件
**位置**: `scheduler.cpp:670`
**问题**: `sigint_received` 全局变量没有原子保护。
**建议**: 使用 `std::atomic<bool>`。

### 9.4 数组越界风险
**位置**: `scheduler.cpp:215`
**问题**: `syscall_info[NP]` 假设 NP 是编译时常量且足够大。
**建议**: 使用 `std::vector` 或 `std::array`。

### 9.5 未初始化变量
**位置**: `scheduler.cpp:1650`
**问题**: `syscall_info` 数组部分字段可能未初始化。
**建议**: 使用值初始化 `{}`。

### 9.6 死锁风险
**位置**: `state_store.cpp` 多处
**问题**: 复杂锁顺序可能导致死锁。
**建议**: 使用锁层次或 std::scoped_lock。

### 9.7 异常安全
**位置**: `state.cpp:625`
**问题**: `new sys_state` 后异常可能导致内存泄漏。
**建议**: 使用智能指针。

### 9.8 类型转换不安全
**位置**: `scheduler.cpp:488`
**问题**: C 风格类型转换 `(void *)ret.args[0]`。
**建议**: 使用 C++ 类型转换 `reinterpret_cast`。

---

## 十、建议的优先级排序

### 高优先级（立即修复）
1. 修复 `state_to_be_discarded()` 死代码 (1.7)
2. 将 `sigint_received` 改为原子类型 (9.3)
3. 修复 `assert` 使用问题 (5.1)
4. 统一错误返回类型 (5.2)
5. 使用智能指针管理 `sys_state` (5.5, 9.7)

### 中优先级（短期重构）
6. 提取 `exploration_step()` 消除重复 (2.1)
7. 封装全局 `ptmc_state` (4.1)
8. 实现策略模式重构探索模式 (6.1)
9. 拆分 `PTMC_STATE` 大结构体 (6.2)
10. 抽象 UI 接口解耦 (1.6)

### 低优先级（长期改进）
11. 统一命名风格 (3.2)
12. 添加单元测试接口 (8.2)
13. 实现事件驱动架构 (6.5)
14. 优化内存分配策略 (7.1)
15. 添加背压机制 (7.3)

---

## 附录：推荐的文件结构调整

```
src/core/
├── scheduler.cpp          # 精简为协调器
├── exploration/
│   ├── exploration_engine.h    # 探索引擎接口
│   ├── bfs_strategy.cpp        # BFS 实现
│   ├── dfs_strategy.cpp        # DFS 实现
│   └── rand_strategy.cpp       # Random 实现
├── state/
│   ├── state_manager.h         # 状态管理接口
│   ├── state_graph.cpp         # 状态图 (queue/set/tree)
│   └── state_transition.cpp    # 状态迁移封装
└── context/
    ├── simulation_context.h     # 全局状态封装
    └── runtime_config.cpp       # 运行时配置
```

---

*报告生成时间: 2026-03-21*
*审查范围: src/core/*.cpp, src/core/*.h*
*发现问题: 50+ 处*
