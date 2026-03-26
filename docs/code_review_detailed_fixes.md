# detsim core/ 目录代码审查报告 - 详细修改方案

## 一、模块边界问题 (Module Boundary Issues)

### 1.1 scheduler.cpp 越界管理状态

**问题**: scheduler 直接操作状态，应该委托给 state 模块

**当前代码** (scheduler.cpp:1193-1222):
```cpp
// BFS 中的状态恢复和保存
auto t_restore_start = std::chrono::high_resolution_clock::now();
s.recover_running_state();
auto t_restore_end = std::chrono::high_resolution_clock::now();
g_restore_time_us += ...

// ... 执行 syscall ...

ptmc_state.dest_state = sys_state(syscall_info);
auto t_save_start = std::chrono::high_resolution_clock::now();
ptmc_state.dest_state.save_metadata();
auto t_save_end = std::chrono::high_resolution_clock::now();
g_save_time_us += ...
state_tree_add(&s, &ptmc_state.dest_state, i, ptmc_state.n_choose ? ptmc_state.choose : -1);
```

**修改方案**: 创建 `StateTransition` 类封装状态迁移

**新增文件**: `src/core/state_transition.h`
```cpp
#ifndef __STATE_TRANSITION_H
#define __STATE_TRANSITION_H

#include "state.h"
#include <functional>

// 状态迁移结果
struct TransitionResult {
    bool success;
    sys_state new_state;
    std::chrono::microseconds restore_time;
    std::chrono::microseconds save_time;
    std::string error_msg;
};

// 状态迁移管理器
class StateTransition {
public:
    // 执行单步状态迁移
    static TransitionResult execute(
        const sys_state& source_state,
        int process_index,
        const syscall_info& syscall,
        std::function<int(syscall_info*)> executor  // exec_once 包装器
    );
    
    // 恢复运行状态（带计时）
    static bool restore_with_timing(sys_state& state, 
                                     int process_index,
                                     std::chrono::microseconds& out_time);
    
    // 保存状态（带计时）
    static bool save_with_timing(sys_state& state,
                                  std::chrono::microseconds& out_time);
};

#endif
```

**新增文件**: `src/core/state_transition.cpp`
```cpp
#include "state_transition.h"
#include "debug.h"

TransitionResult StateTransition::execute(
    const sys_state& source_state,
    int process_index,
    const syscall_info& syscall,
    std::function<int(syscall_info*)> executor)
{
    TransitionResult result;
    
    // 1. 恢复状态
    auto t1 = std::chrono::high_resolution_clock::now();
    sys_state mutable_source = source_state;  // 需要可变副本
    mutable_source.recover_running_state();
    auto t2 = std::chrono::high_resolution_clock::now();
    result.restore_time = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1);
    
    // 2. 执行 syscall
    syscall_info syscall_copy = syscall;
    int ckpt = executor(&syscall_copy);
    
    // 3. 构造新状态
    syscall_info new_syscalls[NP];
    memcpy(new_syscalls, source_state.child, sizeof(new_syscalls));
    new_syscalls[process_index] = syscall_copy;
    
    result.new_state = sys_state(new_syscalls);
    
    // 4. 保存状态
    auto t3 = std::chrono::high_resolution_clock::now();
    bool saved = result.new_state.save_metadata();
    auto t4 = std::chrono::high_resolution_clock::now();
    result.save_time = std::chrono::duration_cast<std::chrono::microseconds>(t4 - t3);
    
    if (!saved) {
        result.success = false;
        result.error_msg = "Failed to save state metadata";
        return result;
    }
    
    // 5. 添加到状态树
    state_tree_add(&source_state, &result.new_state, process_index, 
                   syscall.nr ? syscall.nr : -1);
    
    result.success = true;
    return result;
}
```

**scheduler.cpp 修改后**:
```cpp
// BFS 中的使用
TransitionResult result = StateTransition::execute(
    s, i, syscall_info[i],
    [](syscall_info* si) { return exec_once(si); }
);

if (!result.success) {
    LOG_ERROR("State transition failed: %s", result.error_msg.c_str());
    // 处理错误...
}

g_restore_time_us += result.restore_time.count();
g_save_time_us += result.save_time.count();

// 使用 result.new_state 继续后续逻辑
ptmc_state.dest_state = result.new_state;
```

---

### 1.2 state.cpp 反向依赖 ptmc_state

**问题**: `recover_running_state()` 直接修改全局 `ptmc_state`

**当前代码** (state.cpp:589-598):
```cpp
void tracee_state::recover_running_state(int index) {
    // ... 恢复内存等 ...
    
    /* Restore subsystems */
    ptmc_state.sock_states[index] = sock_state;
    ptmc_state.fs_states[index] = fs_state;
    ptmc_state.raft_states[index] = raft_state;
    
    /* Restore FdManager state */
    *ptmc_state.fd_managers[index] = fd_manager_state;
    
    /* Re-link FdManager */
    ptmc_state.sock_states[index].set_fd_manager(ptmc_state.fd_managers[index]);
    ptmc_state.fs_states[index].set_fd_manager(ptmc_state.fd_managers[index]);
    
    /* Restore time */
    ptmc_state.time[index] = tv;
}
```

**修改方案**: 通过参数传递恢复目标

**修改后** (state.cpp):
```cpp
// 新增结构体保存恢复结果
struct RecoveryTarget {
    SockState* sock_state;
    FileSystemState* fs_state;
    raft_check_state* raft_state;
    std::shared_ptr<FdManager> fd_manager;
    struct timeval* time;
};

void tracee_state::recover_running_state(int index, RecoveryTarget* target) {
    // ... 恢复内存等 ...
    
    if (target) {
        /* Restore subsystems */
        *target->sock_state = sock_state;
        *target->fs_state = fs_state;
        *target->raft_state = raft_state;
        
        /* Restore FdManager state */
        *target->fd_manager = fd_manager_state;
        
        /* Re-link FdManager */
        target->sock_state->set_fd_manager(target->fd_manager);
        target->fs_state->set_fd_manager(target->fd_manager);
        
        /* Restore time */
        *target->time = tv;
    }
}
```

**调用处修改** (scheduler.cpp):
```cpp
// 在恢复状态前准备目标结构
RecoveryTarget target;
target.sock_state = &ptmc_state.sock_states[i];
target.fs_state = &ptmc_state.fs_states[i];
target.raft_state = &ptmc_state.raft_states[i];
target.fd_manager = ptmc_state.fd_managers[i];
target.time = &ptmc_state.time[i];

s.child[i].recover_running_state(i, &target);
```

---

### 1.3 scheduler 直接操作 StateStore 队列

**问题**: scheduler 直接调用 StateStore 的队列操作

**当前代码** (scheduler.cpp:1138-1139):
```cpp
StateStore::instance().queue_clear();
state_queue_append(&ptmc_state.dest_state);
```

**修改方案**: 封装队列操作

**新增文件**: `src/core/state_queue_manager.h`
```cpp
#ifndef __STATE_QUEUE_MANAGER_H
#define __STATE_QUEUE_MANAGER_H

#include "state.h"

class StateQueueManager {
public:
    static StateQueueManager& instance();
    
    void clear();
    void append(sys_state* state);
    void append_front(sys_state* state);
    sys_state* extract();
    bool empty() const;
    size_t size() const;
    
    // BFS/DFS/Rand 模式特定的队列策略
    void setup_for_bfs();
    void setup_for_dfs();
    void setup_for_random();
    
private:
    StateQueueManager() = default;
};

#endif
```

**实现**:
```cpp
#include "state_queue_manager.h"
#include "state_store.h"

StateQueueManager& StateQueueManager::instance() {
    static StateQueueManager mgr;
    return mgr;
}

void StateQueueManager::clear() {
    StateStore::instance().queue_clear();
    // 清空本地队列...
}

void StateQueueManager::append(sys_state* state) {
    state_queue_append(state);  // 现有的 C 函数
}

sys_state* StateQueueManager::extract() {
    return state_queue_extract();  // 现有的 C 函数
}
```

**scheduler.cpp 修改**:
```cpp
// 替换前
StateStore::instance().queue_clear();
state_queue_append(&ptmc_state.dest_state);

// 替换后
StateQueueManager::instance().setup_for_bfs();  // 或 dfs/rand
StateQueueManager::instance().append(&ptmc_state.dest_state);
```

---

### 1.4 init_state() 跨越多个职责

**问题**: 函数同时处理 fork、信号、FdManager 初始化、状态记录

**当前代码** (scheduler.cpp:1647-1699):
```cpp
void init_state() {
    // 1. 信号设置
    setbuf(stdout, NULL);
    atexit(exit_all);
    signal(SIGTERM, exit_all);
    
    // 2. fork 进程
    pids[0] = fork();
    for (int i = 0; i < NP - 1; i++) {
        if (pids[i])
            pids[i + 1] = fork();
        else
            start_tracee(i);
    }
    // ...
    
    // 3. FdManager 初始化
    for (int i = 0; i < NP; i++) {
        ptmc_state.fd_managers[i] = std::make_shared<FdManager>();
        ptmc_state.fs_states[i].set_fd_manager(ptmc_state.fd_managers[i]);
        ptmc_state.sock_states[i].set_fd_manager(ptmc_state.fd_managers[i]);
    }
    
    // 4. tracee 状态初始化
    for (int i = 0; i < NP; i++) {
        ptmc_state.cursor = i;
        init_tracee_state(i);
    }
    
    // 5. 记录首个状态
    struct syscall_info syscall_info[NP];
    memset(syscall_info, 0, sizeof(syscall_info));
    ptmc_state.cursor = -1;
    ptmc_state.dest_state = sys_state(syscall_info);
    ptmc_state.dest_state.save_metadata();
    state_queue_append(&ptmc_state.dest_state);
    ptmc_state.state = PTMC_STOP;
}
```

**修改方案**: 拆分为多个函数

**修改后**:
```cpp
// 1. 信号和进程管理
static void setup_signal_handlers() {
    setbuf(stdout, NULL);
    atexit(exit_all);
    signal(SIGTERM, exit_all);
}

// 2. 进程 fork
static void fork_tracee_processes() {
    pids[0] = fork();
    for (int i = 0; i < NP - 1; i++) {
        if (pids[i])
            pids[i + 1] = fork();
        else
            start_tracee(i);
    }
    
    if (!pids[NP - 1])
        start_tracee(NP - 1);
    
    detsim::ui::ui_printf("Instance #pid = %d\n", getpid());
}

// 3. 子系统初始化
static void initialize_subsystems() {
    for (int i = 0; i < NP; i++) {
        ptmc_state.fd_managers[i] = std::make_shared<FdManager>();
        ptmc_state.fs_states[i].set_fd_manager(ptmc_state.fd_managers[i]);
        ptmc_state.sock_states[i].set_fd_manager(ptmc_state.fd_managers[i]);
    }
}

// 4. tracee 初始化
static void initialize_tracees() {
    for (int i = 0; i < NP; i++) {
        ptmc_state.cursor = i;
        init_tracee_state(i);
    }
}

// 5. 记录初始状态
static void record_initial_state() {
    struct syscall_info syscall_info[NP];
    memset(syscall_info, 0, sizeof(syscall_info));
    ptmc_state.cursor = -1;
    ptmc_state.dest_state = sys_state(syscall_info);
    ptmc_state.dest_state.save_metadata();
    state_queue_append(&ptmc_state.dest_state);
    ptmc_state.state = PTMC_STOP;
}

// 主函数变得简洁
void init_state() {
    setup_signal_handlers();
    fork_tracee_processes();
    initialize_subsystems();
    initialize_tracees();
    record_initial_state();
}
```

---

### 1.5 check_state() 位置不当

**问题**: 状态检查逻辑在 scheduler 中，应该移到 state 模块

**当前代码** (scheduler.cpp:561-587):
```cpp
static int check_state() {
    // 检查各种状态条件...
    for (auto &assertion : ptmc_state.assertions) {
        // ... 检查断言
    }
    // ...
}
```

**修改方案**: 移到 sys_state 类中

**修改后** (state.h):
```cpp
typedef struct sys_state {
    // ... 现有字段 ...
    
    // 新增：状态验证
    enum class CheckResult {
        VALID,
        ASSERTION_FAILED,
        INVALID_STATUS,
        HASH_MISMATCH
    };
    
    CheckResult validate(const std::unordered_set<std::string>& assertions,
                         const std::vector<int (*)()>& user_checks) const;
    
    // 验证单个进程状态
    bool is_process_dead(int index) const {
        return DISDEAD(status[index]);
    }
} sys_state;
```

**实现** (state.cpp):
```cpp
sys_state::CheckResult sys_state::validate(
    const std::unordered_set<std::string>& assertions,
    const std::vector<int (*)()>& user_checks) const
{
    // 检查断言
    for (auto& assertion : assertions) {
        // ... 断言检查逻辑 ...
        if (!evaluate_assertion(assertion, *this)) {
            return CheckResult::ASSERTION_FAILED;
        }
    }
    
    // 检查用户自定义检查
    for (auto check_func : user_checks) {
        if (check_func() != 0) {
            return CheckResult::INVALID_STATUS;
        }
    }
    
    return CheckResult::VALID;
}
```

**scheduler.cpp 调用**:
```cpp
auto result = ptmc_state.dest_state.validate(ptmc_state.assertions, ptmc_state.user_checks);
if (result != sys_state::CheckResult::VALID) {
    // 处理无效状态
}
```

---

### 1.6 UI 与核心逻辑混合

**问题**: NCursesUI 提示直接嵌入 syscall 处理

**当前代码** (scheduler.cpp:239-265):
```cpp
if (!is_auto_mode() && ptmc_state.n_choose != 0) {
    detsim::ui::NCursesUI *ui = get_ncurses_ui();
    if (ui) {
        char prompt_buf[256];
        snprintf(prompt_buf, sizeof(prompt_buf), "Choose from %d options",
                 ptmc_state.n_choose);
        ptmc_state.choose = ui->prompt_int(prompt_buf, 0, 0, ptmc_state.n_choose - 1);
    } else {
        // readline 路径...
    }
}
```

**修改方案**: 抽象用户选择接口

**新增文件**: `src/core/user_choice_provider.h`
```cpp
#ifndef __USER_CHOICE_PROVIDER_H
#define __USER_CHOICE_PROVIDER_H

// 用户选择提供者接口
class IUserChoiceProvider {
public:
    virtual ~IUserChoiceProvider() = default;
    
    // 从 n 个选项中选择一个
    virtual int choose(int n_options, const char* context) = 0;
    
    // 批量模式预设
    virtual void set_preset_choice(int choice) = 0;
    virtual int get_preset_choice() const = 0;
};

// NCurses UI 实现
class NCursesChoiceProvider : public IUserChoiceProvider {
public:
    int choose(int n_options, const char* context) override;
    void set_preset_choice(int choice) override { preset_ = choice; }
    int get_preset_choice() const override { return preset_; }
    
private:
    int preset_ = -1;
};

// Readline 实现（无 UI 模式）
class ReadlineChoiceProvider : public IUserChoiceProvider {
public:
    int choose(int n_options, const char* context) override;
    void set_preset_choice(int choice) override { preset_ = choice; }
    int get_preset_choice() const override { return preset_; }
    
private:
    int preset_ = -1;
};

// 自动模式（随机/确定性选择）
class AutoChoiceProvider : public IUserChoiceProvider {
public:
    enum class Strategy { RANDOM, SEQUENTIAL };
    
    AutoChoiceProvider(Strategy s = Strategy::RANDOM) : strategy_(s) {}
    int choose(int n_options, const char* context) override;
    void set_preset_choice(int choice) override {}
    int get_preset_choice() const override { return -1; }
    
private:
    Strategy strategy_;
    int sequential_counter_ = 0;
};

#endif
```

**scheduler.cpp 修改**:
```cpp
// 全局或成员变量
std::unique_ptr<IUserChoiceProvider> choice_provider;

// 初始化时根据模式设置
void setup_choice_provider() {
    if (is_auto_mode()) {
        choice_provider = std::make_unique<AutoChoiceProvider>();
    } else if (get_ncurses_ui()) {
        choice_provider = std::make_unique<NCursesChoiceProvider>();
    } else {
        choice_provider = std::make_unique<ReadlineChoiceProvider>();
    }
    choice_provider->set_preset_choice(ptmc_state.batch_choice_preset);
}

// 使用
static int on_syscall_exit(pid_t pid, struct syscall_info *info) {
    ptmc_state.n_choose = analyze_choose(info);
    
    if (ptmc_state.n_choose > 0) {
        int preset = choice_provider->get_preset_choice();
        if (preset >= 0 && preset < ptmc_state.n_choose) {
            ptmc_state.choose = preset;
            choice_provider->set_preset_choice(-1);
        } else {
            ptmc_state.choose = choice_provider->choose(
                ptmc_state.n_choose, 
                "syscall choice"
            );
        }
    }
    // ...
}
```

---

### 1.7 state_to_be_discarded() 死代码

**问题**: 函数开头直接 `return 0`，后续代码永远不会执行

**当前代码** (scheduler.cpp:1098-1114):
```cpp
bool state_to_be_discarded(int index, syscall_info *infos) {
    return 0;  // <-- 死代码从这里开始
    if (index == 0) {
        uintptr_t raft = eval<uintptr_t>("((raft_server_private_t *)raft)", 0);
        if (raft == 0)
            return false;
        int state = eval<int>("((raft_server_private_t *)raft)->state", 0);
        if (state == 2 || state == 3) {
            return true;
        }
    }
    return false;
}
```

**修改方案**: 修复逻辑或删除

**方案 A - 如果确实需要该功能**:
```cpp
bool state_to_be_discarded(int index, syscall_info *infos) {
    // 移除了错误的 return 0
    
    // 只对主进程检查 raft 状态
    if (index != 0) {
        return false;
    }
    
    // 获取 raft 服务器指针
    uintptr_t raft = eval<uintptr_t>("((raft_server_private_t *)raft)", 0);
    if (raft == 0) {
        // 如果无法评估，不丢弃
        return false;
    }
    
    // 获取 raft 状态
    int raft_state = eval<int>("((raft_server_private_t *)raft)->state", 0);
    
    // 状态 2 (CANDIDATE) 或 3 (LEADER) 时丢弃
    // 注释说明原因：这些状态可能是不稳定中间状态
    if (raft_state == 2 || raft_state == 3) {
        LOG_DEBUG("Discarding state with raft state %d", raft_state);
        return true;
    }
    
    return false;
}
```

**方案 B - 如果不需要该功能**:
```cpp
// 直接删除整个函数
// 并删除所有调用处：
// if (!state_to_be_discarded(i, syscall_info)) { ... }
// 改为直接执行逻辑
```

---

## 二、代码重复问题 (Code Duplication)

### 2.1 状态恢复/保存模板重复

**问题**: 三个探索模式有相同的状态恢复-执行-保存模式

**当前代码** (BFS/DFS/Rand 中重复):
```cpp
// BFS 版本 (scheduler.cpp:1193-1222)
auto t_restore_start = std::chrono::high_resolution_clock::now();
s.recover_running_state();
auto t_restore_end = std::chrono::high_resolution_clock::now();
g_restore_time_us += ...

for (int j = 0; j < NP; j++)
    syscall_info[j] = s.child[j].si;

int ckpt = exec_once(syscall_info + i);

ptmc_state.dest_state = sys_state(syscall_info);
auto t_save_start = std::chrono::high_resolution_clock::now();
ptmc_state.dest_state.save_metadata();
auto t_save_end = std::chrono::high_resolution_clock::now();
g_save_time_us += ...
state_tree_add(&s, &ptmc_state.dest_state, i, ptmc_state.n_choose ? ptmc_state.choose : -1);
```

**修改方案**: 提取模板函数

**新增函数** (scheduler.cpp):
```cpp
// 探索步骤结果
struct ExplorationStep {
    sys_state new_state;
    int checkpoint_result;
    std::chrono::microseconds restore_time;
    std::chrono::microseconds save_time;
    bool success;
};

// 通用探索步骤
static ExplorationStep execute_exploration_step(
    sys_state& source_state,
    int process_index,
    std::function<int(syscall_info*)> executor)
{
    ExplorationStep result;
    
    // 1. 恢复状态
    auto t1 = std::chrono::high_resolution_clock::now();
    source_state.recover_running_state();
    auto t2 = std::chrono::high_resolution_clock::now();
    result.restore_time = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1);
    
    // 2. 准备 syscall 信息
    syscall_info syscall_info[NP];
    for (int j = 0; j < NP; j++) {
        syscall_info[j] = source_state.child[j].si;
    }
    
    // 3. 执行
    result.checkpoint_result = executor(&syscall_info[process_index]);
    
    // 4. 构造和保存新状态
    result.new_state = sys_state(syscall_info);
    auto t3 = std::chrono::high_resolution_clock::now();
    result.success = result.new_state.save_metadata();
    auto t4 = std::chrono::high_resolution_clock::now();
    result.save_time = std::chrono::duration_cast<std::chrono::microseconds>(t4 - t3);
    
    // 5. 添加到状态树
    if (result.success) {
        state_tree_add(&source_state, &result.new_state, process_index,
                       result.new_state.child[process_index].si.nr ? 
                       result.new_state.child[process_index].si.nr : -1);
    }
    
    return result;
}

// BFS 中使用
ExplorationStep step = execute_exploration_step(s, i, 
    [](syscall_info* si) { return exec_once(si); });

g_restore_time_us += step.restore_time.count();
g_save_time_us += step.save_time.count();
ptmc_state.dest_state = step.new_state;

// DFS 中使用
ExplorationStep step = execute_exploration_step(s, i, 
    [](syscall_info* si) { return exec_once(si); });
// ...

// Rand 中使用
ExplorationStep step = execute_exploration_step(s, i, 
    [](syscall_info* si) { return exec_once(si); });
// ...
```

---

### 2.2 状态统计更新重复

**问题**: 相同的统计更新逻辑在多处复制

**当前代码**:
```cpp
// BFS: scheduler.cpp:1267-1270
g_states_searched = states_searched_this_run;
g_states_new = states_new_this_run;
g_queue_size = StateStore::instance().queue_size();

// Rand: scheduler.cpp:1599-1601  
g_states_searched = states_searched_this_run;
g_states_new = states_new_this_run;
```

**修改方案**: 封装统计更新

**新增类**:
```cpp
class ExplorationStatistics {
public:
    void update(size_t searched, size_t new_states) {
        g_states_searched = searched;
        g_states_new = new_states;
    }
    
    void update_queue_size() {
        g_queue_size = StateStore::instance().queue_size();
    }
    
    void increment_searched() {
        g_states_searched++;
    }
    
    void increment_new() {
        g_states_new++;
    }
    
    void reset() {
        g_states_searched = 0;
        g_states_new = 0;
        g_queue_size = 0;
    }
};

// 使用
ExplorationStatistics stats;
stats.update(searched_this_run, new_this_run);
stats.update_queue_size();
```

---

### 2.3 SIGINT 处理重复

**问题**: 中断处理逻辑几乎完全相同

**当前代码** (scheduler.cpp:1272-1296 和 1605-1630):
```cpp
if (sigint_received) {
    stop_status_monitor();
    detsim::ui::ui_printf("Program received signal SIGINT, Interrupt.\n");
    detsim::ui::ui_printf("Flushing pending state writes...\n");
    StateStore::instance().wait_for_completion();
    SysStateStore::instance().flush_index();
    detsim::ui::ui_printf("Searched for %zu sys_states...", ...);
    // ... 更多重复代码
    return 0;
}
```

**修改方案**: 提取函数

```cpp
static int handle_interrupt(size_t states_searched, size_t states_new) {
    stop_status_monitor();
    detsim::ui::ui_printf("Program received signal SIGINT, Interrupt.\n");
    
    // 刷新状态
    detsim::ui::ui_printf("Flushing pending state writes...\n");
    StateStore::instance().wait_for_completion();
    SysStateStore::instance().flush_index();
    
    // 打印统计
    detsim::ui::ui_printf(
        "Searched for %zu sys_states (new: %zu), total unique: %zu\n",
        states_searched, states_new, state_set.size()
    );
    
    if (ptmc_state.dest_state.ss_hash == 0) {
        LOG_CRIT("Current state hash is 0...");
    }
    
    show_syscall_history();
    stop_status_monitor();
    
    double time_used = gettime() - g_start_time;
    detsim::ui::ui_printf("Time elapsed: %lfs, speed = %lf states/s\n",
                          time_used, states_searched / time_used);
    StateStore::instance().print_stats();
    
    sigint_received = 0;
    state_set.clear();
    
    return 0;
}

// 使用
if (sigint_received) {
    return handle_interrupt(states_searched_this_run, states_new_this_run);
}
```

---

### 2.4 探索模式状态机重复

**问题**: 模式选择 switch 在多处出现

**当前代码** (scheduler.cpp:1163-1173, 1022-1026):
```cpp
// 多处出现的模式判断
switch (ptmc_state.mode) {
    case PTMC_STATE::MODE_RAND:
        indexes.push_back(rand() % NP);
        break;
    default:
        for (int k = 0; k < NP; k++)
            indexes.push_back(k);
        shuffle(indexes.begin(), indexes.end(), ...);
}

// 另一处
if (ptmc_state.mode == PTMC_STATE::MODE_RAND)
    mode_str = "RAND";
else if (ptmc_state.mode == PTMC_STATE::MODE_DFS)
    mode_str = "DFS";
else if (ptmc_state.mode == PTMC_STATE::MODE_BFS)
    mode_str = "BFS";
```

**修改方案**: 使用 Strategy 模式（见 6.1 详细方案）

---

### 2.5 起始/结束报告重复

**问题**: 结果输出逻辑重复

**当前代码**:
```cpp
// BFS 结束
stop_status_monitor();
detsim::ui::ui_printf("Complete explore all %zu sys_states...", ...);
show_syscall_history();
double time_used = gettime() - start_time;
detsim::ui::ui_printf("Time elapsed: %lfs...", ...);
StateStore::instance().print_stats();

// Rand 结束 - 几乎相同
stop_status_monitor();
detsim::ui::ui_printf("Complete explore all %zu sys_states...", ...);
show_syscall_history();
double time_used = gettime() - start_time;
detsim::ui::ui_printf("Time elapsed: %lfs...", ...);
StateStore::instance().print_stats();
```

**修改方案**: 提取报告函数

```cpp
static void report_exploration_summary(const char* mode_name,
                                        size_t searched,
                                        size_t new_states,
                                        double start_time) {
    stop_status_monitor();
    detsim::ui::ui_printf(
        "[%s] Complete explore all %zu sys_states (new: %zu), total unique: %zu\n",
        mode_name, searched, new_states, state_set.size()
    );
    show_syscall_history();
    
    double time_used = gettime() - start_time;
    detsim::ui::ui_printf("Time elapsed: %lfs, speed = %lf states/s\n",
                          time_used, searched / time_used);
    StateStore::instance().print_stats();
}

// 使用
report_exploration_summary("BFS", states_searched_this_run, 
                          states_new_this_run, start_time);
```

---

### 2.6 状态验证逻辑重复

**问题**: `check_state()` 调用和错误处理重复

**修改方案**: 统一封装（见 1.5）

---

## 三、命名与可读性问题

### 3.1 模糊的类型别名

**问题**: `LSS`, `SSS`, `TSS` 含义不清

**当前代码** (state.cpp:667-669):
```cpp
extern LSS state_queue;
extern SSS state_set;
extern TSS state_tree;
```

**修改方案**:
```cpp
// 使用有意义的名称
using StateQueue = std::deque<hash_type>;
using StateHashSet = std::unordered_set<hash_type>;
using StateParentTree = std::unordered_map<hash_type, 
                          std::tuple<hash_type, int, int>>;

extern StateQueue g_state_queue;
extern StateHashSet g_state_set;
extern StateParentTree g_state_tree;
```

---

### 3.2 不一致的命名风格

**问题**: 混用 snake_case 和 camelCase

**修改方案**: 统一为 snake_case（C++ 标准库风格）

```cpp
// 修改前
struct syscall_info {
    int nr;
    uintptr_t rval;
    uintptr_t args[6];
    
    template <class Archive>
    void serialize(Archive &ar);
};

typedef struct tracee_state {
    hash_type ts_hash;
    syscall_info si;
    // ...
} tracee_state;

// 修改后 - 保持一致
struct SyscallInfo {
    int nr;
    uintptr_t rval;
    uintptr_t args[6];
    
    template <class Archive>
    void serialize(Archive &ar);
};

struct TraceeState {
    hash_type ts_hash;
    SyscallInfo syscall_info;
    // ...
};
```

---

### 3.3 缩写不清晰

**问题**: `ptmc`, `CKPT`, `DISDEAD` 含义不明

**修改方案**:
```cpp
// ptmc_state -> simulation_state 或 sim_state
// PTMC_PRELOAD -> SimState::PRELOAD
// CKPT_NO -> Checkpoint::CONTINUE
// CKPT_YES -> Checkpoint::SAVE
// CKPT_DISCARD -> Checkpoint::DISCARD
// DISDEAD(status) -> is_process_dead(status)

// 定义清晰的枚举
enum class CheckpointAction {
    CONTINUE,    // CKPT_NO
    SAVE,        // CKPT_YES
    DISCARD,     // CKPT_DISCARD
    EXIT,        // CKPT_EXIT
    STOP         // CKPT_STOP
};

enum class ProcessStatus {
    RUNNING,
    EXITED,
    CRASHED,
    STOPPED
};

inline bool is_process_dead(int status) {
    return WIFEXITED(status) || WIFSIGNALED(status);
}
```

---

### 3.4 魔术数字

**问题**: `114514` 等魔术数字

**当前代码**:
```cpp
if (ret.nr == SYS_set_tid_address) {
    ret.rval = 114514;
    tracee_set_rax(pid, 114514);
}
```

**修改方案**:
```cpp
// 定义常量
namespace DummyValues {
    constexpr uint64_t TID_ADDRESS_RETURN = 0x1BF52;  // 114514
    constexpr uint64_t RSEQ_COOKIE = 0xDEADBEEF;
}

// 使用
if (syscall.nr == SYS_set_tid_address) {
    syscall.return_value = DummyValues::TID_ADDRESS_RETURN;
    tracee_set_rax(pid, DummyValues::TID_ADDRESS_RETURN);
}
```

---

### 3.5 变量名与类型名冲突

**问题**: `syscall_info syscall_info[NP]` 变量名与类型名相同

**修改方案**:
```cpp
// 修改前
syscall_info syscall_info[NP];

// 修改后
SyscallInfo syscall_infos[NP];
// 或
std::array<SyscallInfo, NP> syscall_array;
```

---

### 3.6 状态命名混淆

**问题**: `source_state`, `dest_state`, `current_state` 含义不同

**修改方案**:
```cpp
// 使用更清晰的命名
ptmc_state.parent_state;    // source_state - 迁移前的状态
ptmc_state.child_state;     // dest_state - 迁移后的状态
ptmc_state.initial_state;   // 初始状态
ptmc_state.current_state;   // 当前正在处理的状态

// 或使用 BFS/DFS 术语
ptmc_state.frontier_state;  // 待探索的状态（队列中的）
ptmc_state.visited_state;   // 已探索的状态
```

---

### 3.7 函数命名不一致

**问题**: `exec_bfs()` vs `do_dfs()` vs `exec_rand()`

**修改方案**:
```cpp
// 统一命名风格
int explore_bfs();           // exec_bfs
int explore_dfs(int depth);  // do_dfs -> exec_dfs
int explore_random(int depth); // exec_rand

// 或使用策略模式（推荐）
class ExplorationStrategy {
public:
    virtual ~ExplorationStrategy() = default;
    virtual int explore(int depth = 0) = 0;
    virtual const char* name() const = 0;
};

class BFSStrategy : public ExplorationStrategy {
public:
    int explore(int depth = 0) override;
    const char* name() const override { return "BFS"; }
};
```

---

## 四、全局状态与耦合问题

### 4.1 过度使用的全局变量 ptmc_state

**问题**: 整个核心模块依赖单一全局状态对象

**修改方案**: 封装为 SimulationContext 类

**新增文件**: `src/core/simulation_context.h`
```cpp
#ifndef __SIMULATION_CONTEXT_H
#define __SIMULATION_CONTEXT_H

#include "state.h"
#include <memory>

class SimulationContext {
public:
    static SimulationContext& instance();
    
    // 禁止拷贝
    SimulationContext(const SimulationContext&) = delete;
    SimulationContext& operator=(const SimulationContext&) = delete;
    
    // 状态访问
    sys_state& parent_state() { return parent_state_; }
    sys_state& child_state() { return child_state_; }
    const sys_state& parent_state() const { return parent_state_; }
    const sys_state& child_state() const { return child_state_; }
    
    // 探索控制
    enum class Mode { DFS, BFS, RANDOM };
    Mode exploration_mode() const { return mode_; }
    void set_mode(Mode m) { mode_ = m; }
    
    int current_process() const { return cursor_; }
    void set_current_process(int c) { cursor_ = c; }
    
    // 选择相关
    int num_choices() const { return n_choose_; }
    void set_num_choices(int n) { n_choose_ = n; }
    int current_choice() const { return choose_; }
    void set_current_choice(int c) { choose_ = c; }
    
    // 进程状态
    int process_status(int index) const { return status_[index]; }
    void set_process_status(int index, int status) { status_[index] = status; }
    bool is_process_dead(int index) const;
    
    // 子系统访问
    SockState& socket_state(int index) { return sock_states_[index]; }
    FileSystemState& filesystem_state(int index) { return fs_states_[index]; }
    std::shared_ptr<FdManager> fd_manager(int index) { return fd_managers_[index]; }
    
    // 初始化
    void initialize_subsystems();
    void shutdown();
    
private:
    SimulationContext() = default;
    
    // 探索状态
    Mode mode_ = Mode::BFS;
    int run_state_ = 0;  // PTMC_PRELOAD, etc.
    int cursor_ = -1;
    int n_choose_ = 0;
    int choose_ = -1;
    
    // 系统状态
    sys_state parent_state_;
    sys_state child_state_;
    hash_type sysstate_hash_ = 0;
    
    // 进程状态
    int status_[NP] = {};
    pid_t pids_[NP] = {};
    
    // 子系统
    SockState sock_states_[NP];
    std::shared_ptr<FdManager> fd_managers_[NP];
    FileSystemState fs_states_[NP];
    struct timeval time_[NP];
    
    // Raft 状态
    raft_check_state raft_states_[NP];
};

#endif
```

**使用方式**:
```cpp
// 替换前
ptmc_state.mode = PTMC_STATE::MODE_BFS;
ptmc_state.cursor = i;
ptmc_state.dest_state.save_metadata();

// 替换后
auto& ctx = SimulationContext::instance();
ctx.set_mode(SimulationContext::Mode::BFS);
ctx.set_current_process(i);
ctx.child_state().save_metadata();
```

---

### 4.2 全局信号处理器依赖全局状态

**问题**: `exit_all()` 直接访问全局状态

**修改方案**:
```cpp
// 使用安全的信号处理
class SignalHandler {
public:
    static void setup();
    static void request_shutdown();
    static bool is_shutdown_requested();
    
private:
    static std::atomic<bool> shutdown_requested_;
    static void handle_signal(int sig);
};

// 实现
std::atomic<bool> SignalHandler::shutdown_requested_{false};

void SignalHandler::setup() {
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
}

void SignalHandler::handle_signal(int sig) {
    shutdown_requested_.store(true);
}

void SignalHandler::request_shutdown() {
    shutdown_requested_.store(true);
    kill(0, SIGTERM);
}

// 在主循环中检查
while (!SignalHandler::is_shutdown_requested()) {
    // 探索循环
}
```

---

### 4.3 extern 声明分散

**问题**: 多个 extern 声明散布在文件各处

**修改方案**: 集中到头文件

**新增文件**: `src/core/extern_decls.h`
```cpp
#ifndef __EXTERN_DECLS_H
#define __EXTERN_DECLS_H

// UI
extern "C" detsim::ui::NCursesUI* get_ncurses_ui();
extern FILE* log_fp;

// 配置
extern int auto_mode;
extern char* batch_file;

// 运行时数据结构
extern std::unordered_map<int, void*> rseq_struct;
extern std::unordered_map<int, int> rseq_len;

// 状态容器
extern StateQueue g_state_queue;
extern StateHashSet g_state_set;
extern StateParentTree g_state_tree;

// 信号
extern std::atomic<bool> g_sigint_received;

// 统计
extern std::atomic<size_t> g_states_searched;
extern std::atomic<size_t> g_states_new;

#endif
```

---

### 4.4 静态全局变量

**问题**: `g_depth_stat_file`, `g_fit_a` 等静态全局变量

**修改方案**: 封装到统计类

```cpp
class DepthStatistics {
public:
    void start_recording(const std::string& filename);
    void stop_recording();
    void record_depth(size_t depth, size_t total_states);
    
    // 指数拟合
    bool fit_exponential(double& out_a, double& out_b);
    
private:
    FILE* stat_file_ = nullptr;
    std::vector<std::pair<double, double>> data_points_;
    double fit_a_ = 0.0;
    double fit_b_ = 0.0;
};

class ExplorationCounters {
public:
    void reset();
    void increment_searched();
    void increment_new();
    
    size_t searched_this_run() const { return searched_; }
    size_t new_this_run() const { return new_; }
    
private:
    size_t searched_ = 0;
    size_t new_ = 0;
};
```

---

### 4.5 模式判断通过全局枚举

**问题**: `ptmc_state.mode` 是全局状态的一部分

**修改方案**: 使用策略对象（见 6.1）

---

### 4.6 状态队列全局可访问

**问题**: `state_queue`, `state_set`, `state_tree` 全局可访问

**修改方案**: 封装到 StateGraph 类

```cpp
class StateGraph {
public:
    static StateGraph& instance();
    
    // 队列操作
    void enqueue(hash_type state_hash);
    hash_type dequeue();
    bool queue_empty() const;
    size_t queue_size() const;
    
    // 集合操作
    bool contains(hash_type state_hash) const;
    void insert(hash_type state_hash);
    size_t unique_count() const;
    
    // 树操作
    void record_transition(hash_type from, hash_type to, 
                           int process, int choice);
    std::vector<hash_type> get_path(hash_type state) const;
    
    void clear();
    
private:
    StateQueue queue_;
    StateHashSet set_;
    StateParentTree tree_;
};
```

---

## 五、错误处理问题

### 5.1 assert 用于运行时检查

**问题**: 使用 assert 检查 ptrace 结果

**当前代码**:
```cpp
assert(WIFSTOPPED(wstatus) && WSTOPSIG(wstatus) == PTRACE_TRAP_SIG);
assert(info.op == PTRACE_SYSCALL_INFO_ENTRY);
```

**修改方案**:
```cpp
// 替换为显式错误处理
bool check_ptrace_stop(int wstatus, const char* context) {
    if (!WIFSTOPPED(wstatus)) {
        LOG_ERROR("[%s] Process not stopped: wstatus=%d", context, wstatus);
        return false;
    }
    int sig = WSTOPSIG(wstatus);
    if (sig != PTRACE_TRAP_SIG) {
        LOG_ERROR("[%s] Unexpected signal: %d (expected %d)", 
                  context, sig, PTRACE_TRAP_SIG);
        return false;
    }
    return true;
}

// 使用
if (!check_ptrace_stop(wstatus, "syscall entry")) {
    return -1;  // 或抛出异常
}
```

---

### 5.2 不一致的错误返回类型

**问题**: 混用 `bool`, `int`, `void` 返回错误状态

**修改方案**: 使用 `Result<T>` 类型

**新增文件**: `src/core/result.h`
```cpp
#ifndef __RESULT_H
#define __RESULT_H

#include <variant>
#include <string>

template<typename T>
class Result {
public:
    static Result<T> ok(T value) {
        return Result(std::move(value));
    }
    
    static Result<T> err(std::string msg) {
        return Result(std::move(msg));
    }
    
    bool is_ok() const { return std::holds_alternative<T>(data_); }
    bool is_err() const { return std::holds_alternative<Error>(data_); }
    
    T& value() { return std::get<T>(data_); }
    const T& value() const { return std::get<T>(data_); }
    
    const std::string& error() const { return std::get<Error>(data_).msg; }
    
    T value_or(T default_val) const {
        return is_ok() ? value() : default_val;
    }
    
private:
    struct Error { std::string msg; };
    std::variant<T, Error> data_;
    
    explicit Result(T val) : data_(std::move(val)) {}
    explicit Result(Error err) : data_(std::move(err)) {}
};

// 特化 void
class VoidResult {
public:
    static VoidResult ok() { return VoidResult(true); }
    static VoidResult err(std::string msg) { 
        VoidResult r(false);
        r.error_msg_ = std::move(msg);
        return r;
    }
    
    bool is_ok() const { return success_; }
    bool is_err() const { return !success_; }
    const std::string& error() const { return error_msg_; }
    
private:
    bool success_;
    std::string error_msg_;
    explicit VoidResult(bool s) : success_(s) {}
};

#endif
```

**使用示例**:
```cpp
// 修改前
bool read_from_disk(hash_type hash, std::vector<uint8_t>& data);

// 修改后
Result<std::vector<uint8_t>> read_from_disk(hash_type hash);

// 使用
auto result = read_from_disk(hash);
if (result.is_ok()) {
    auto data = result.value();
    // 使用数据
} else {
    LOG_ERROR("Failed to read: %s", result.error().c_str());
}
```

---

### 5.3 静默失败

**问题**: `analyze_choose()` 默认返回 0，可能掩盖错误

**修改方案**:
```cpp
// 修改前
int analyze_choose(struct syscall_info *info) {
    // ...
    return 0;  // 默认返回
}

// 修改后
Result<int> analyze_choose(const syscall_info& info) {
    int predefined = choose_many[info.nr];
    if (predefined > 0) {
        return Result<int>::ok(predefined);
    }
    
    if (info.nr == SYS_recvfrom) {
        int fd = info.args[0];
        int choices = emu_recvfrom_get_choices(fd);
        if (choices >= 2) {
            return Result<int>::ok(2);
        }
        return Result<int>::ok(0);
    }
    
    return Result<int>::ok(0);  // 明确的无选择情况
}
```

---

### 5.4 LOG_CRIT 后继续执行

**问题**: 关键错误后继续执行

**修改方案**:
```cpp
// 修改前
if (s.ts_hash[i] == 0) {
    LOG_CRIT("Child state hash is 0 for tracee %d...", i);
    print_call_stack();
}
// 继续执行...

// 修改后
if (s.ts_hash[i] == 0) {
    LOG_CRIT("Child state hash is 0 for tracee %d...", i);
    print_call_stack();
    return ExplorationError::INVALID_STATE;
    // 或抛出异常
}
```

---

### 5.5 缺少资源清理

**问题**: `state_fetched` 删除前需要清理

**修改方案**: 使用智能指针

```cpp
// 修改前
sys_state *state_fetched = state_queue_extract();
// ... 使用 ...
state_fetched->clear();
delete state_fetched;

// 修改后
auto state_fetched = std::unique_ptr<sys_state>(state_queue_extract());
// ... 使用 ...
// 自动清理，无需手动 delete

// 或者修改 extract 返回智能指针
std::unique_ptr<sys_state> state_queue_extract();
```

---

### 5.6 文件操作无错误检查

**问题**: `g_depth_stat_file` 可能为 nullptr

**修改方案**:
```cpp
// 使用 RAII 包装
class FileGuard {
    FILE* fp_;
public:
    explicit FileGuard(FILE* fp) : fp_(fp) {}
    ~FileGuard() { if (fp_) fclose(fp_); }
    
    FILE* get() const { return fp_; }
    bool is_open() const { return fp_ != nullptr; }
    
    // 禁止拷贝
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    
    // 允许移动
    FileGuard(FileGuard&& other) : fp_(other.fp_) { other.fp_ = nullptr; }
};

// 使用
FileGuard file(fopen(filename, "a"));
if (!file.is_open()) {
    LOG_ERROR("Failed to open %s", filename);
    return;
}
fprintf(file.get(), "...");
```

---

### 5.7 内存分配失败未处理

**问题**: `new` 可能抛出 `std::bad_alloc`

**修改方案**:
```cpp
// 使用 nothrow 版本检查
sys_state* state = new (std::nothrow) sys_state(hash);
if (!state) {
    LOG_CRIT("Failed to allocate sys_state");
    return nullptr;
}

// 或使用智能指针自动处理
try {
    auto state = std::make_unique<sys_state>(hash);
    // ...
} catch (const std::bad_alloc& e) {
    LOG_CRIT("Memory allocation failed: %s", e.what());
    return nullptr;
}
```

---

## 六、设计模式问题

### 6.1 缺乏策略模式

**问题**: DFS/BFS/Rand 通过 if/switch 分支实现

**修改方案**: 实现策略模式

**新增文件**: `src/core/exploration_strategy.h`
```cpp
#ifndef __EXPLORATION_STRATEGY_H
#define __EXPLORATION_STRATEGY_H

#include "state.h"

// 探索结果
struct ExplorationResult {
    bool completed;
    size_t states_searched;
    size_t states_new;
    std::string error_msg;
};

// 策略接口
class IExplorationStrategy {
public:
    virtual ~IExplorationStrategy() = default;
    
    // 执行探索
    virtual ExplorationResult explore(int depth) = 0;
    
    // 策略名称
    virtual const char* name() const = 0;
    
    // 设置起始状态
    void set_initial_state(const sys_state& state) {
        initial_state_ = state;
    }
    
protected:
    sys_state initial_state_;
};

// BFS 策略
class BFSStrategy : public IExplorationStrategy {
public:
    ExplorationResult explore(int depth) override;
    const char* name() const override { return "BFS"; }
    
private:
    void process_state(sys_state* state);
};

// DFS 策略
class DFSStrategy : public IExplorationStrategy {
public:
    ExplorationResult explore(int depth) override;
    const char* name() const override { return "DFS"; }
    
private:
    int do_dfs_recursive(hash_type state_hash, int depth);
};

// Random 策略
class RandomStrategy : public IExplorationStrategy {
public:
    ExplorationResult explore(int depth) override;
    const char* name() const override { return "Random"; }
    
private:
    std::mt19937 rng_{std::random_device{}()};
};

// 策略工厂
class StrategyFactory {
public:
    static std::unique_ptr<IExplorationStrategy> create(const std::string& mode);
};

#endif
```

**BFS 实现示例**:
```cpp
ExplorationResult BFSStrategy::explore(int depth) {
    ExplorationResult result;
    
    StateQueueManager::instance().setup_for_bfs();
    StateQueueManager::instance().append(&initial_state_);
    
    std::unique_ptr<sys_state> state;
    while ((state = std::unique_ptr<sys_state>(
            StateQueueManager::instance().extract())) != nullptr) {
        
        process_state(state.get());
        result.states_searched++;
        
        if (SignalHandler::is_shutdown_requested()) {
            break;
        }
    }
    
    result.completed = true;
    return result;
}

void BFSStrategy::process_state(sys_state* state) {
    // 处理每个进程
    std::vector<int> indexes = get_process_order(state);
    
    for (int i : indexes) {
        if (state->is_process_dead(i)) continue;
        
        ExplorationStep step = execute_exploration_step(
            *state, i, 
            [](syscall_info* si) { return exec_once(si); }
        );
        
        if (!step.success) {
            LOG_ERROR("Step failed");
            continue;
        }
        
        // 检查是否新状态
        if (!StateGraph::instance().contains(step.new_state.ss_hash)) {
            StateGraph::instance().insert(step.new_state.ss_hash);
            StateQueueManager::instance().append(&step.new_state);
        }
    }
}
```

**使用方式**:
```cpp
// scheduler.cpp 中的修改
int run_exploration(const std::string& mode, int depth) {
    auto strategy = StrategyFactory::create(mode);
    if (!strategy) {
        LOG_ERROR("Unknown mode: %s", mode.c_str());
        return -1;
    }
    
    strategy->set_initial_state(ptmc_state.dest_state);
    
    start_status_monitor();
    auto result = strategy->explore(depth);
    stop_status_monitor();
    
    // 输出结果
    report_exploration_summary(result, strategy->name());
    
    return result.completed ? 0 : 1;
}
```

---

### 6.2 庞大的状态结构体

**问题**: `PTMC_STATE` 包含 30+ 字段

**修改方案**: 拆分为多个结构体（见 4.1 SimulationContext）

---

### 6.3 缺少访问者模式

**问题**: 状态序列化逻辑分散

**修改方案**: 使用访问者模式统一处理序列化

```cpp
class StateSerializer {
public:
    virtual ~StateSerializer() = default;
    virtual void serialize(const sys_state& state, std::ostream& out) = 0;
    virtual std::unique_ptr<sys_state> deserialize(std::istream& in) = 0;
};

class CerealSerializer : public StateSerializer {
public:
    void serialize(const sys_state& state, std::ostream& out) override {
        // 使用 cereal 序列化
    }
    
    std::unique_ptr<sys_state> deserialize(std::istream& in) override {
        // 反序列化
    }
};

class StateSerializerRegistry {
public:
    void register_serializer(const std::string& format, 
                             std::unique_ptr<StateSerializer> serializer);
    StateSerializer* get(const std::string& format);
};
```

---

### 6.4 紧耦合的 syscalls 处理

**问题**: `on_syscall_enter()` 是大 switch 语句

**修改方案**: 使用命令模式

```cpp
// 系统调用处理器接口
class ISyscallHandler {
public:
    virtual ~ISyscallHandler() = default;
    virtual bool handle_enter(pid_t pid, const syscall_info& info) = 0;
    virtual bool handle_exit(pid_t pid, const syscall_info& info) = 0;
    virtual int syscall_number() const = 0;
};

// 具体处理器示例
class GettimeofdayHandler : public ISyscallHandler {
public:
    int syscall_number() const override { return SYS_gettimeofday; }
    
    bool handle_enter(pid_t pid, const syscall_info& info) override {
        // 模拟的系统调用，直接返回
        tracee_set_rax(pid, 0);
        return true;  // 已处理，不需要真实系统调用
    }
    
    bool handle_exit(pid_t pid, const syscall_info& info) override {
        return true;
    }
};

// 处理器注册表
class SyscallDispatcher {
public:
    void register_handler(std::unique_ptr<ISyscallHandler> handler);
    
    bool dispatch_enter(pid_t pid, const syscall_info& info) {
        auto it = handlers_.find(info.nr);
        if (it != handlers_.end()) {
            return it->second->handle_enter(pid, info);
        }
        return false;  // 未处理，需要真实系统调用
    }
    
private:
    std::unordered_map<int, std::unique_ptr<ISyscallHandler>> handlers_;
};

// 初始化
void init_syscall_handlers() {
    auto& dispatcher = SyscallDispatcher::instance();
    dispatcher.register_handler(std::make_unique<GettimeofdayHandler>());
    dispatcher.register_handler(std::make_unique<NanosleepHandler>());
    // ... 更多处理器
}
```

---

### 6.5 缺少观察者模式

**问题**: 状态变更直接调用 UI 更新

**修改方案**: 使用事件/观察者模式

```cpp
// 事件类型
enum class ExplorationEvent {
    STATE_TRANSITION,
    CHOICE_REQUIRED,
    ASSERTION_FAILED,
    EXPLORATION_COMPLETE,
    DEPTH_REACHED
};

// 事件数据
struct EventData {
    ExplorationEvent type;
    hash_type from_state;
    hash_type to_state;
    int process_index;
    std::string message;
};

// 观察者接口
class IExplorationObserver {
public:
    virtual ~IExplorationObserver() = default;
    virtual void on_event(const EventData& event) = 0;
};

// UI 观察者
class UIObserver : public IExplorationObserver {
public:
    void on_event(const EventData& event) override {
        switch (event.type) {
            case ExplorationEvent::CHOICE_REQUIRED:
                prompt_user_choice(event);
                break;
            case ExplorationEvent::ASSERTION_FAILED:
                show_error_dialog(event);
                break;
            // ...
        }
    }
};

// 统计观察者
class StatisticsObserver : public IExplorationObserver {
public:
    void on_event(const EventData& event) override {
        if (event.type == ExplorationEvent::STATE_TRANSITION) {
            stats_.record_transition();
        }
    }
};

// 事件总线
class ExplorationEventBus {
public:
    void subscribe(std::shared_ptr<IExplorationObserver> observer);
    void publish(const EventData& event);
    
private:
    std::vector<std::weak_ptr<IExplorationObserver>> observers_;
};
```

---

## 七、性能与资源管理问题

### 7.1 频繁的内存分配

**问题**: `new sys_state()` 频繁分配

**修改方案**: 使用对象池

```cpp
template<typename T>
class ObjectPool {
public:
    ObjectPool(size_t initial_size = 100) {
        for (size_t i = 0; i < initial_size; i++) {
            pool_.push(std::make_unique<T>());
        }
    }
    
    std::unique_ptr<T> acquire() {
        if (pool_.empty()) {
            return std::make_unique<T>();
        }
        auto obj = std::move(pool_.top());
        pool_.pop();
        return obj;
    }
    
    void release(std::unique_ptr<T> obj) {
        obj->clear();  // 重置状态
        pool_.push(std::move(obj));
    }
    
private:
    std::stack<std::unique_ptr<T>> pool_;
};

// 使用
ObjectPool<sys_state> state_pool(1000);

// 获取对象
auto state = state_pool.acquire();
// 使用...
state_pool.release(std::move(state));
```

---

### 7.2 重复的文件系统操作

**问题**: `/proc/{pid}/maps` 每次捕获都重新打开

**修改方案**: 缓存文件描述符或使用 /proc/[pid]/map_files

```cpp
class ProcessMemoryInfo {
public:
    explicit ProcessMemoryInfo(pid_t pid) : pid_(pid) {
        maps_path_ = fmt::format("/proc/{}/maps", pid);
    }
    
    std::vector<MemoryRegion> get_regions() {
        // 使用缓存或增量读取
        auto now = std::chrono::steady_clock::now();
        if (now - last_read_ > refresh_interval_) {
            refresh();
            last_read_ = now;
        }
        return cached_regions_;
    }
    
private:
    void refresh();
    
    pid_t pid_;
    std::string maps_path_;
    std::vector<MemoryRegion> cached_regions_;
    std::chrono::steady_clock::time_point last_read_;
    std::chrono::milliseconds refresh_interval_{100};
};
```

---

### 7.3 无限制的队列增长

**问题**: BFS 模式下 `state_queue` 可能无限增长

**修改方案**: 添加背压机制

```cpp
class BoundedStateQueue {
public:
    explicit BoundedStateQueue(size_t max_size) : max_size_(max_size) {}
    
    bool try_enqueue(hash_type state) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.size() >= max_size_) {
            // 背压：等待消费者
            cv_full_.wait(lock, [this] { 
                return queue_.size() < max_size_; 
            });
        }
        queue_.push_back(state);
        cv_empty_.notify_one();
        return true;
    }
    
    hash_type dequeue() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_empty_.wait(lock, [this] { return !queue_.empty(); });
        
        auto state = queue_.front();
        queue_.pop_front();
        cv_full_.notify_one();
        return state;
    }
    
private:
    std::deque<hash_type> queue_;
    size_t max_size_;
    std::mutex mutex_;
    std::condition_variable cv_full_;
    std::condition_variable cv_empty_;
};
```

---

### 7.4 定时器精度问题

**问题**: `time(NULL)` 精度只有秒级

**修改方案**: 使用 `std::chrono`

```cpp
// 高精度计时器
class HighResolutionTimer {
public:
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;
    
    void start() { start_ = Clock::now(); }
    
    double elapsed_seconds() const {
        auto now = Clock::now();
        return std::chrono::duration<double>(now - start_).count();
    }
    
    int64_t elapsed_microseconds() const {
        auto now = Clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(
            now - start_).count();
    }
    
private:
    TimePoint start_;
};

// 替代 time(NULL)
class MonotonicClock {
public:
    static int64_t now_seconds() {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
    }
};
```

---

## 八、测试与可维护性问题

### 8.1 函数过长

**问题**: `exec_bfs()` ~200 行

**修改方案**: 提取辅助函数（见 2.1, 2.2 等）

---

### 8.2 缺少单元测试接口

**问题**: 核心逻辑与全局状态强耦合

**修改方案**: 依赖注入

```cpp
// 测试友好的接口
class IStateStore {
public:
    virtual ~IStateStore() = default;
    virtual hash_type save(const void* data, size_t len) = 0;
    virtual ssize_t load(hash_type hash, std::vector<uint8_t>& out) = 0;
    virtual bool exists(hash_type hash) = 0;
};

// 真实实现
class RealStateStore : public IStateStore {
    // 包装 StateStore::instance()
};

// Mock 实现用于测试
class MockStateStore : public IStateStore {
public:
    MOCK_METHOD(hash_type, save, (const void* data, size_t len), (override));
    MOCK_METHOD(ssize_t, load, (hash_type hash, std::vector<uint8_t>& out), (override));
    MOCK_METHOD(bool, exists, (hash_type hash), (override));
};

// 探索引擎接受注入
class ExplorationEngine {
public:
    explicit ExplorationEngine(IStateStore* store) : store_(store) {}
    
    void run() {
        // 使用 store_ 而不是 StateStore::instance()
    }
    
private:
    IStateStore* store_;
};

// 测试
TEST(ExplorationTest, BasicBFS) {
    MockStateStore mock_store;
    EXPECT_CALL(mock_store, save(_, _)).WillRepeatedly(Return(12345));
    
    ExplorationEngine engine(&mock_store);
    engine.run();
}
```

---

### 8.3 代码注释不足

**问题**: 复杂算法缺少说明

**修改方案**: 添加文档注释

```cpp
/**
 * 更新深度统计的指数拟合
 * 
 * 使用指数模型: N(d) = a * exp(b * d)
 * 其中:
 *   N(d) 是在深度 d 的总状态数
 *   a    是初始系数
 *   b    是增长率
 * 
 * 使用最小二乘法拟合对数转换后的数据:
 *   ln(N) = ln(a) + b * d
 * 
 * @param depth 当前探索深度
 * @param total_states 总状态数
 * @return true 如果拟合成功
 * 
 * @see https://en.wikipedia.org/wiki/Exponential_growth
 */
bool update_depth_fit(size_t depth, size_t total_states);
```

---

## 九、具体小错误

### 9.1 条件编译宏使用不当

**问题**: `#define pids ptmc_state.pids` 宏可能产生副作用

**修改方案**: 使用内联函数

```cpp
// 删除宏定义
// #define pids ptmc_state.pids

// 使用内联函数
inline pid_t* get_pids() { return ptmc_state.pids; }
inline pid_t get_pid(int index) { return ptmc_state.pids[index]; }
inline void set_pid(int index, pid_t pid) { ptmc_state.pids[index] = pid; }
```

---

### 9.2 潜在的整数溢出

**问题**: `states_searched_this_run` 是 `int` 可能溢出

**修改方案**:
```cpp
// 修改前
static int states_searched_this_run = 0;

// 修改后  
static std::atomic<size_t> states_searched_this_run{0};
// 或
static std::atomic<uint64_t> states_searched_this_run{0};
```

---

### 9.3 竞态条件

**问题**: `sigint_received` 全局变量没有原子保护

**修改方案**:
```cpp
// 修改前
extern int sigint_received;

// 修改后
extern std::atomic<bool> g_sigint_received;

// 信号处理器
void sigint_handler(int) {
    g_sigint_received.store(true, std::memory_order_relaxed);
}

// 检查
if (g_sigint_received.load(std::memory_order_relaxed)) {
    // 处理中断
}
```

---

### 9.4 数组越界风险

**问题**: `syscall_info[NP]` 假设 NP 足够大

**修改方案**: 使用 `std::array`

```cpp
// 修改前
syscall_info syscall_info[NP];

// 修改后
std::array<syscall_info, NP> syscall_array;

// 或动态大小
std::vector<syscall_info> syscall_vec(NP);

// 访问时检查
if (index >= syscall_array.size()) {
    throw std::out_of_range("Process index out of range");
}
```

---

### 9.5 未初始化变量

**问题**: `syscall_info` 数组部分字段可能未初始化

**修改方案**:
```cpp
// 修改前
struct syscall_info syscall_info[NP];
memset(syscall_info, 0, sizeof(syscall_info));

// 修改后 - 使用值初始化
std::array<syscall_info, NP> syscall_info{};  // {} 零初始化

// 或显式初始化
syscall_info syscall_info[NP] = {};  // C 风格零初始化
```

---

### 9.6 死锁风险

**问题**: 复杂锁顺序可能导致死锁

**修改方案**: 使用 `std::scoped_lock` (C++17)

```cpp
// 修改前
mutex1.lock();
mutex2.lock();  // 可能的死锁

// 修改后
std::mutex mutex1, mutex2;

void safe_operation() {
    std::scoped_lock lock(mutex1, mutex2);  // 原子同时锁定
    // 安全操作
}

// 或 C++11/14 使用 std::lock
void safe_operation() {
    std::unique_lock<std::mutex> lock1(mutex1, std::defer_lock);
    std::unique_lock<std::mutex> lock2(mutex2, std::defer_lock);
    std::lock(lock1, lock2);  // 无死锁锁定
}
```

---

### 9.7 异常安全

**问题**: `new sys_state` 后异常可能导致内存泄漏

**修改方案**: 使用智能指针

```cpp
// 修改前
sys_state *s = new sys_state(hash);
// ... 可能抛出异常 ...
delete s;

// 修改后
auto s = std::make_unique<sys_state>(hash);
// ... 可能抛出异常 ...
// 自动释放
```

---

### 9.8 类型转换不安全

**问题**: C 风格类型转换 `(void *)ret.args[0]`

**修改方案**: 使用 C++ 类型转换

```cpp
// 修改前
void* addr = (void*)ret.args[0];

// 修改后
void* addr = reinterpret_cast<void*>(static_cast<uintptr_t>(ret.args[0]));

// 或更安全的封装
class GuestAddress {
public:
    explicit GuestAddress(uintptr_t addr) : addr_(addr) {}
    
    template<typename T>
    T as() const {
        return reinterpret_cast<T>(static_cast<uintptr_t>(addr_));
    }
    
    uintptr_t raw() const { return addr_; }
    
private:
    uintptr_t addr_;
};

// 使用
GuestAddress addr(info.args[0]);
void* ptr = addr.as<void*>();
```

---

## 总结

以上修改方案涵盖了报告中提出的 50+ 个问题，每个问题都提供了：

1. **问题描述** - 说明为什么这是个问题
2. **当前代码** - 展示现有实现（如有必要）
3. **修改方案** - 提供具体的代码实现
4. **使用示例** - 展示如何在新代码中使用

### 实施建议

**阶段 1 - 高优先级（立即修复）**:
1. 修复 `state_to_be_discarded()` 死代码
2. 将 `sigint_received` 改为原子类型
3. 修复 `assert` 使用问题
4. 统一错误返回类型
5. 使用智能指针管理 `sys_state`

**阶段 2 - 中优先级（短期重构）**:
6. 提取 `exploration_step()` 消除重复
7. 封装全局 `ptmc_state` 
8. 实现策略模式重构探索模式
9. 拆分 `PTMC_STATE` 大结构体
10. 抽象 UI 接口解耦

**阶段 3 - 低优先级（长期改进）**:
11. 统一命名风格
12. 添加单元测试接口
13. 实现事件驱动架构
14. 优化内存分配策略
15. 添加背压机制

