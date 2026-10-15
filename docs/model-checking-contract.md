# 状态与 atom 契约评估（2026-10-14）

基于代码提交 `072671c`，结合独立只读审查和下述实测。以下主体保留修复前证据；后续修复状态见下一段。
结论：当前实现有可用的受控执行骨架，但**尚不满足一般原生二进制模型检验的
状态正确性与交织覆盖要求**。Redis 固定路径成功、简单计数器回放成功，不能
作为这两个契约成立的证明。

## 首轮修复进展

已补上每线程用户 XSTATE（XMM/AVX/x87/MXCSR）、只读私有页及映射权限恢复、
严格快照 I/O、包含节点顺序/status/budget 的系统状态身份。旧 v3 快照明确拒绝。
原 XMM 反例现在重复回放均为 11；内存变量恢复后为 0。恢复触发 VMA 合并造成的
假 hash 差异也已有规范化修复，计数器重放保留原 hash 断言。

syscall 参数在 enter handler 前填充；自动执行以每个已完成 syscall 为边界，
不再吞掉 CKPT_NO 循环。TCP 临时端口只依赖可保存的 socket 状态。

这些修复不代表一般模型检验已经成立：共享/匿名 guard 之外的不可读映射与缺失文件 backing 明确
拒绝，线程/futex 保存、actor 枚举、共享内存细粒度边界、时钟/随机/信号仍需修复。
具体测试及剩余范围以 [correctness-backlog.md](correctness-backlog.md) 为准。

后续 futex 修复：等待队列改为分支独立所有，保存/恢复同步重建 blocked 状态；
WAIT 保留后继，WAKE/REQUEUE 修正线程选择与 syscall 参数。线程槽在捕获时保留，
受限 raw clone/exit 可跨逻辑线程创建与退出恢复；未知物理线程变化及完整 pthread 生命周期仍不支持。当前快照为 v17（flat）/v18（pages），旧版本不会被默认为完整状态。
同 hash 快速路径也恢复模型元数据。一般线程生命周期恢复与交织枚举仍未完成。

## 需要保证什么

设 S 为模型状态，a 为选定 actor 的动作，o 为显式环境结果。至少需要：

1. `step(restore(S), a, o)` 的结果不依赖之前跑过哪个分支。
2. 若两个快照被合并，它们对待检性质具有等价的未来行为；仅字节哈希相同不够。
3. `enabled(S)` 包含模型允许的线程/进程动作和环境结果，且不包含不可能的动作。
4. 被合成 atom 的内部动作不隐藏相关交织、阻塞和性质观察；不能因为实现方便而合并。
5. 遇到不支持的状态/操作、恢复失败或内部执行不收敛，应报告模型不适用或搜索未完成。
   不能将这些情况报告为“没有发现错误”。

“存对”不要求保存机器上每个内核字节，但必须保存或可靠重建所有影响未来
可观察行为的状态。模拟 OS 可以替代真实内核资源，但那份模型必须有明确语义。

## 一、控制、保存和恢复

### 已实测反例：XMM 状态跨分支泄漏

使用一个不经过 C 函数调用的内联汇编片段：

```c
volatile long observed = 0;
int main(void) {
    __asm__ volatile(
        "mov $11, %%rax\n\tmovq %%rax, %%xmm15\n\t"
        "mov $39, %%rax\n\tsyscall\n\t" // x86-64 Linux getpid，保存 S
        "movq %%xmm15, observed(%%rip)\n\t"
        "mov $22, %%rax\n\tmovq %%rax, %%xmm15\n\t"
        "mov $39, %%rax\n\tsyscall\n\t" // 保存后继
        "movq %%xmm15, observed(%%rip)\n\t"
        "mov $39, %%rax\n\tsyscall\n\t"
        : : : "rax", "rcx", "r11", "xmm15", "memory");
    return 0;
}
```

用 `/usr/bin/gcc -O0 -g -gdwarf-4 -fno-pie -no-pie` 编译，NP=2 配置中启动
两个相同实例；只对进程 0 操作。第一条 `si` 后从 `info` 取后继哈希 S；
再 `si; p observed`，然后 `load S; si; p observed`。

实际得到 **11 → 恢复 S → 22**。复查时，load 后的普通内存变量先正确回到 0，
但再次执行相同尾段仍得到 22。tracer 正常退出，没有报告恢复失败。
另一个进程始终未被推进。诊断产物在 `build/model-contract-audit/`，其源码、
驱动和 transcript/result 均被保留，但构建目录不是永久存档。

原因明确：`thread_state` 只存 `user_regs_struct`（state.h:79-101），
state.cpp:973/1024 捕获通用寄存器，:565-569/:719 恢复通用寄存器；
没有 FP/SIMD/XSTATE 路径。这个缺口影响单线程，和交织爆炸无关。

### 其他源码确认的缺口（尚未在本轮逐项实测）

| 缺口 | 当前证据 | 后果 / 最小验收 |
| --- | --- | --- |
| 映射内容与属性不完整 | `state.cpp:886-888` 仅保存可写页；`:1378-1392` 只比地址；`guest.cpp:495-499` 重建匿名私有映射 | 只读页此前可修改；mprotect、文件/共享映射不能靠地址匹配恢复。测试写1→RO保存→写2→RO→恢复，内容须回到1；另测权限。 |
| Futex/阻塞状态分离 | `state.cpp:1152-1173` 新捕获初始化空 futex；`:1427-1441` 不恢复 blocked；`state.h:238-242` 默认复制裸指针 | 队列、runnable、等待原因必须作为一个状态保存、复制、恢复。测试 wait→wake→restore，且分支副本不别名。 |
| 线程身份与创建信息丢失 | `state.cpp:967-1000` 重建并编号线程；`:1152-1153` 清空创建记录；`:651-714` 缺 clone 参数/按位置匹配 | 创建 A/B，A 退出再建 C，恢复后必须还是逻辑 A/B，包含正确 TLS/寄存器/停止阶段/等待关系。 |
| 状态 key 不保留语义维度 | `state.cpp:121-174` XOR 子哈希；`:188-216` 同 key 不保存；`serialize.cpp:49-51` 元数据另含 status/budget | 有序节点状态、终止状态、预算必须有明确定义。XOR 丢顺序是代数问题，不是概率性碰撞；具体交换状态是否可达另需验证。 |
| 未受控的宿主输入 | `dispatcher.cpp:72-78,118-122` 未模拟的 syscall 默认透传；`sockstate.cpp:221-228` 用 tracer rand 产生端口 | clock_gettime/getrandom 等没有进入显式 outcome；搜索与被测环境还共享 rand。相同 S/a/o 的后继可能随运行历史改变。 |
| 内核可观察状态未闭合 | `state.h:175-198`、`serialize.cpp:44-45` 没有 signal mask/disposition/pending/timer | 要么建模保存这些状态，要么明确拒绝相关行为；不能默认透传后认为快照完整。 |
| I/O 失败仍发布状态 | `state.cpp:1046,1089-1107,1137` 未检查正数短读、失败填零；`:527-557` 忽略短写；`:1300-1305` 用 live 内存替代缺失快照 | 故障注入短读/短写/缺页/损坏快照；必须失败而不是把零、旧页或其他分支的值当成正确状态。 |

还有一个控制层错误：`exec_engine.cpp:78` 先调用 syscall enter handler，
`:80-83` 才写入当前 syscall 参数；读取 `info.args` 的 clone/futex enter
handler 因而可能读到上一条 syscall 的参数。

## 二、atom 与不确定性覆盖

### 当前实际定义

活动 CLI 使用 `core/scheduler.cpp`，不是另一套 `scheduler/exploration.cpp`。
它在 :702-724 枚举进程，调用 `state_transition(S, process)`。

手动模式一次执行一个 syscall；自动模式在 `exec_engine.cpp:229-243` 对
`CKPT_NO` 继续执行当前线程，直到某个 handler 返回 checkpoint。因此自动 atom
实际上是：**用户态指令 + 若干不取快照的 syscall + 一个取快照的 syscall**。
`CKPT_YES/NO` 不只是存储策略，它已经决定搜索图上哪些交织和中间状态存在。

### 为什么这还不够

- **缺少 actor 枚举。** 正常自动路径不枚举同一进程内每个 runnable 线程。
  executor 自己选择一个线程不等于搜索全部线程分支。
- **共享内存不必经过 syscall。** 下面两线程使用顺序一致原子变量，避免 C/C++
  data-race UB；每段分别是 load 和 store，而不是 fetch_add：

  ```text
  x 初始为 0
  T1: a = x.load(); x.store(a + 1);
  T2: b = x.load(); x.store(b + 1);
  ```

  load/load/store/store 的真实交织允许两线程完成后 x=1。即使补上线程枚举，
  若把每段 load/store 捏成一个 atom，也只能得到终值2。这是粒度的理论反例，
  本轮没有执行这个多线程例子。
- **atom 可能不返回。** `handlers.cpp:60-69` 把无 choices 的 recvfrom(-EAGAIN)
  视为 CKPT_NO；`:568-579` 的 sleep 也立即成功并返回 NO。接收者忙等时可一直
  占着一次 transition，发信进程得不到调度。纯用户态自旋也没有 syscall 边界。
- **阻塞不是无效分支。** futex WAIT 注册队列后返回 `CKPT_DISCARD`
  （handlers.cpp:501-514），BFS 不保留后继（scheduler.cpp:746-754）。正确的
  “线程进入等待”应形成可保存的状态，再选择其他 actor 或环境唤醒。
- **环境 outcomes 被固定或截断。** UDP 接收只选前两条（sockstate.cpp:673-679）；
  futex wake 固定取队列前部；epoll 忽略 timeout。它们可以是受限模型，但不能
  宣称覆盖全部消息投递、唤醒和时间结果。必须区分网络在途消息和已投递接收队列，
  也不能把 TCP 字节流当成可任意重排的独立消息。
- **性质仍在边界采样。** 当前状态/转换检查与 monitor 更新已统一到后继提交前；
  原有 Raft 角色剪枝已删除，error_bound 仍是显式模型预算。atom 内先违例后恢复
  的中间状态依然不可见；遍历有限模型不能扩大为原程序所有行为都安全。
- **停止屏障尚不成立。** clone handler 会放行新线程后直接返回 checkpoint
  （handlers.cpp:396-405,429-439），没有确认新线程再次停止；不能假设保存期间
  只有选定线程在修改内存。

### 什么时候可以合并

atom 需要绑定**程序类别、环境模型和待检性质**。对于被省略调度点的内部
动作（不包括保留在边界上的可见交互），至少应满足：

1. 对其他 actor 不产生相关可观察交互，不读取会与其他 actor 冲突的共享状态；
   或已有可验证的可交换依据。
2. 不改变其他动作的 enabledness；没有被吞掉的等待、唤醒或环境输入。
3. 不隐藏待检性质关心的中间状态/事件。checkpoint invariant 和任意时刻 invariant
   必须区分；前者可允许更粗的 atom。
4. 能结束并归还调度权，或明确报告 divergence/超限；超限不能当成安全结论。

对独立地址空间、单线程节点，全部外部交互都由模型接管时，纯本地计算可以
合并到下一次可见交互。这保留原始设计的价值，但当前实现尚未保证这些前提。
对共享内存多线程二进制，需要冲突访问/同步动作的更细边界，或者限定支持类别；
仅枚举线程或增加 syscall checkpoint 都不够。还必须声明采用的内存模型。

## 建议的实施次序与验收

1. **先确定支持配置。** 第一档限定单线程、独立地址空间、明确 syscall/信号/
   映射/时间模型；不支持的行为显式拒绝。不能把“可运行”当“已纳入模型”。
2. **补状态闭合。** 从本次 XMM 反例开始，补映射、I/O 失败、时钟/随机输入和
   状态身份。每项跑 S→A→S→B，并换搜索顺序、缓存策略，比较语义后继。
3. **显式表示动作。** 把 enabled actors、pending interaction、outcomes 和
   checkpoint policy 分开；可用 `(logical actor, event, outcome)` 描述选择。
   搜索随机数不得改变被测环境；plugin 必须纯函数或注册其可恢复状态。
4. **先建立细粒度参考，再做合并。** 在明确的支持模型内保守设边界，记录读写/
   同步/投递足迹。粗粒度版本必须与参考版本对小程序比较可达结果、死锁和反例。
5. **之后才做线程扩展与缩减。** 先修 clone/等待状态/稳定 TID，再验证线程
   分支覆盖；再考虑有证明依据的合并或偏序缩减。

两个关键指标应替代“跑过几个例子”：**恢复后语义后继一致性**和
**声明模型内的动作/结果覆盖**。在这两点成立之前，没有发现违例不构成安全保证。

## 参考边界

这里的判断主要来自当前源码和 XMM 反例。相关原始工作也明确区分模型的构造、
后继生成和搜索范围，而不是仅以能执行程序为准：
[CMC 的设计说明](https://www.usenix.org/legacy/events/osdi02/tech/full_papers/musuvathi/musuvathi_html/node4.html)。
[CHESS 原始说明](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/02/chess-ec2-submission.pdf)
将调度探索的保证限定在指定抢占界限内；这类显式界限和未声明地漏掉分支不同。
