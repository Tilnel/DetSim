# 案例驱动的 DetSim 验收

后续功能以具体案例的阻塞点和反例为依据。每个案例分别记录原生运行、启动、受控执行、保存恢复、搜索、性质检查和反例重放；不把其中一级替代下一级。

## 运行

```sh
# NP=1：C/C++ 输入分支 + pthread 丢通知，正常/注错对照，full/pages
make check-cases CC=/usr/bin/gcc CXX=/usr/bin/g++

# 六类运行时准入；当前存在真实阻塞，所以预期非零退出，不能当绿色 CI
make audit-runtimes CC=/usr/bin/gcc CXX=/usr/bin/g++

# 单项实验（tracer 必须已按 NP=1 构建）
python3 examples/cases/run.py --mode inputs --backend both
python3 examples/cases/run.py --mode threads --backend both
python3 examples/cases/run.py --mode runtimes --languages rust,go --depth 100
```

runner 只依赖 Python 标准库及已有编译器。不会下载依赖、修改 vendor、关闭宿主 ASLR 或清除旧快照。编译和结果写入新的 `build/cases/run-*` 目录。每次搜索使用新的 tracer 和 store，避免前一次 DFS 的 visited 集合污染下一次搜索。每个 tracer session 有总墙钟截止时间及 2 GiB 地址空间限制；native/编译子进程另有超时与独立进程组。

`results.json` 保留工具链实际版本、编译和启动参数、源码/二进制/plugin/tracer 哈希、显式环境、profile、阶段结果和日志路径。`config.json`、`commands.json`、`transcript.log` 和 `witness.json` 可用于复现。哈希重放验证在同一 tracee 生命周期内进行；新的进程可能有不同地址，不能直接搬用旧状态哈希。

## 已落地的案例

### 两次扣款：C 与 C++

账户初始余额1，两次输入分别决定是否扣款1。正确版每次检查余额；`INJECT_BUG` 仅删除第二次扣款的余额检查。

- 输入通过公开时间环境插件注入：两次 `gettimeofday` 各选择“不前进”或“前进1微秒”。程序计算相邻时间差得到输入位。
- 独立原生 oracle 枚举 `00/01/10/11`；正确结果为余额1或0，注错版仅 `11` 产生余额-1。
- 检查器从保存的 stdout 缓冲读取协议，使用分支私有 monitor 记录输入和终态，不依赖语言 DWARF 或 libc 内部布局。
- 正确版四条手控路径及有限模型 BFS 通过；注错版 BFS 找到透支，恢复前驱后三次重放同一违例边，状态哈希一致。
- full/pages 均验收。此处的完成结论只针对这两个输入位组成的有限模型。

### 丢失通知：真实 pthread

主线程持锁创建 worker，随后等待条件变量；worker 设置谓词并通知。注错版只删除 `pthread_cond_signal`。

- 正确版原生完成，DetSim 内输出 `RUNTIME_OK` 并退出0。
- 注错版搜索发现所有存活线程均等待无 deadline 的 futex，且封闭系统内没有线程能再发出通知；公共 StateView 检查器报告违例。
- full/pages 搜索得到违例，恢复前驱后三次重放同一状态。
- native 注错超时只作对照，不把“超时”本身称为死锁证明。
- 当前使用已有 actor 选择策略，不宣称枚举全部 pthread 交织；这个案例证明能找到该具体反例。

## 运行时准入基线（2026-10-16）

`probes/` 使用真实标准运行时/解释器，原生需输出 `RUNTIME_OK` 并退出0。模型内同样由 plugin 检查保存的输出与退出；tracer exit0、进入提示符、DFS 打印 `Complete explore all` 均不足以判定通过。

| 目标 | 实测工具链 | 启动、两个不同控制状态及恢复重放 | 应用运行/搜索结果 |
| --- | --- | --- | --- |
| C | GCC14.2 | 通过 | 标记及退出0，搜索完成 |
| C++ std | GCC14.2 | 通过 | 标记及退出0，搜索完成 |
| Rust std | rustc1.51.0 | 通过 | full/pages 已完成输出检查及退出0 |
| Go | gccgo14.2（非标准 gc） | 通过 | full/pages 均完成输出/退出及3x重放；full 显式4GiB限额 |
| Python | CPython3.13.5，`-I -S` | 通过 | full/pages 各273状态，输出/退出0及3x重放通过 |
| Java | OpenJDK21.0.12.1 | 通过 | 文件身份和系统动态库输入已补；SIGBUS-backed 映射尚不支持；条件目标 |

这些版本是本机实测，不代表整个语言、其他版本或所有库已受支持。Java 保留正常 JVM/JIT，只显式限制 heap/code cache/class space；没有把 JVM 私有结构补丁放进框架。标准 gc Go 的单独准入目标尚待可用工具链；不能用 gccgo 结果代替它。

本次案例推动了三个通用修复：动态 argv（原数组在5参数写终止符时越界，9参数直接拒绝）、匿名零长度 mmap 的正常 `EINVAL` 路径、`poll(timeout=0)`。文件 mmap 的 `EBADF` 优先级和 huge-page 不支持边界仍保留；阻塞 poll 仍明确拒绝。

后续这一批补了 `sched_getaffinity` 固定虚拟 CPU0 查询，以及 `readlink/readlinkat`：链接目标复用 VFS 节点内容保存，初始化捕获 executable 路径链和 `/proc/self/exe`，探索期间不查询宿主路径。普通链接遍历仍明确拒绝；未支持完整 procfs 或 CPU 拓扑。真实 syscall 的 full/pages 三次恢复重放另有回归。内核模型版本升为 v12，快照布局不变。

为正确描述 executable 文件，当前捕获其完整字节，会增加 VFS 复制与哈希成本。12 组小案例单次总耗时旧版12.43秒、新版12.19秒（含编译），小案例未显示明显退化，但 Redis SET/GET 场景从先前7.691秒变为26.012秒，约3.38倍，属于实际性能回退。原因是现有 VFS 用独占 vector 保存文件，静态 executable 字节也随每个状态复制、哈希和序列化；后续应统一改为不可变内容块和分支写覆盖。不能把这称为 readlink 的必然成本。gccgo 的内存失败也单独保留，不以另一次 pages 的进展掩盖。

### VFS 回退已修复

静态内容现在由不可变 payload 共享，≥4KiB 内容在生产快照中保存 StateStore 引用；写入时分离，普通独立归档仍内联完整字节。没有丢弃 executable 内容。格式更新为 flat25/pages26。

同配置 Redis SET/GET 固定调度各跑三次，全部为 server208步、client58步且响应正确：

| 模式 | 修改前秒数 | 修改后秒数 | 中位数变化 | 峰值RSS中位数 | 存储量中位数 |
| --- | --- | --- | --- | --- | --- |
| full | 27.114 / 26.592 / 26.845 | 7.553 / 7.318 / 7.659 | 26.845 → 7.553 | 471.9 → 319.0 MiB | 2158.6 → 599.7 MiB |
| pages | 19.514 / 19.281 / 19.506 | 2.024 / 1.980 / 1.891 | 19.506 → 1.980 | 510.9 → 168.1 MiB | 1577.6 → 17.3 MiB |

这是固定调度场景的全链路测量，不是一般状态空间探索加速保证。原始记录在 `build/vfs-comparison/results.json`。20个原生场景（含大文件修改/恢复）、12组正常/注错案例与共享/冷恢复/损坏引用回归通过。大文件首次修改仍整文件复制，尚无文件分块。

## 接下来的推进顺序

1. 用上述未通过的 probe 驱动 `sigaltstack`、`access`、只读运行环境文件及大规模保留映射捕获的支持。每修一处先让原案例继续，再记录下一个阻塞；不要靠放宽未知 syscall passthrough 或跳过初始化来变绿。
2. 为各语言加入正常/注错小协议，保持相同的证据等级，而不仅是 Hello World。
3. 接入 willemt Raft 与现有 RedisRaft **库驱动**，分别定义 leader/term/commit 性质及可解释的复制构建 mutation。已有 `benchmark-redisraft` 不是完整 Redis+RedisRaft module 的证明。
4. 再推进完整 RedisRaft、真实多线程/多进程系统。只有具体案例要求时再扩展 actor/timeout/outcome 枚举和共享内存切换边界。

候选目标与状态见 [catalog.json](catalog.json)。已知限制必须保持为 unsupported、bounded、timeout 或 failed，不能统计成已验证安全。`case_runner_integration.py` 专门验证深度不足和缺失工具链不会变绿。


准入迭代补充：sigaltstack 注册/查询为每逻辑线程状态，物理 carrier 保持禁用，
固定虚拟最小栈2048；不声称支持 handler delivery。FSMap 现在能导入单文件、
目录和链接，并显式报告输入失败；F_OK 查保存的命名空间，凭据相关模式仍拒绝。
当前格式flat27/pages28、kernelv13。runtime runner 为 Python/JDK 保存显式
rootfs manifest及输入哈希；解释器性质使用协议观察，不假定DWARF能解释对象堆。
Java若需要重大JVM私有机制工作，可以保留为明确未通过的研究目标；不可伪造通过。


DFS 不再为每层祖先长期持有完整文件系统内容。64MiB VFS、44步前缀及晚分叉
回归在不变的2GiB限额下完成八组BFS/DFS对照，均81个唯一状态、80条转换、
四个终态，修改边与终端边均三次重放；未改 visited/选择策略。Python实际运行
已越过旧内存失败，继续暴露目录枚举接口缺口。


零FD pselect6 已作为 pending timer 支持，普通captured symlink也已支持，kernelv14。
runner新增 `--memory-mb`，默认仍2048，报告保留实际限额；gccgo full工作集实验
显式使用4096，90秒只走到约100状态而非分配失败，不能算通过。所有成功搜索
现在额外重放终点前驱三次，保留 `completion.json`；bounded/timeout仍非绿色。


kernelv15 增加 getdents64、统一非零 inode 和可配置时钟原点（默认1秒），以及
Linux已忽略的两个历史mmap标志。getdents原生故障/填充矩阵、真实目录游标恢复、
两份最小共享库分别加载及默认/零/自定义时钟测试通过。gccgo pages已到正常终点
并3x恢复重放；full原始探索也到exit0，但原试验驱动丢失，不能据此补造完整验收。
Python下一步先修输入文件mtime被抹成0的问题，再判断是否仍需rename语义。


当前保存格式为flat29/pages30。`info current`只读标量边元数据，runner以
`bfs --quiet`/`dfs N --quiet`搜索，避免自动历史反复读回完整大快照。
独立24组full/pages正常/静默搜索测试保留相同转移与三次重放。

Python/Java的语义性质优先观察显式输出、消息协议和外部可见状态，不依赖
DWARF解释解释器/JVM对象。Java继续保留为条件目标：当前真实阻塞是SIGBUS-backed
文件映射，需要映射、快照和故障语义的共同改造，不能仅放开mmap标志。
它仍报告未通过，不阻塞其他案例；不以关闭JIT/GC冒充常规JVM支持。


kernelv17复用已有FD_CLOEXEC状态支持FIOCLEX/FIONCLEX，并按Linux ABI截取命令
低32位，忽略参数但保持EBADF优先级。原生/实际模型调用覆盖状态标志、宿主
隔离及复制/归档恢复。Python full/pages准入均通过（273状态），不代表所有
Python程序已可检验。gccgo full也已完成控制、探索、退出和终点3x重放（237状态，
显式4GiB，整轮约222秒）；pages独立通过，不能据此替代标准Go gc。

标准gc独立试验使用官方SHA256校验的Go1.23.12工具链，隔离在忽略的build目录。
其默认munmap启动边界晚于线程创建；显式改为mmap边界后控制/恢复通过，
实际搜索停在MADV_NOHUGEPAGE，尚不计绿色。官方工具链清单及校验依据：
https://go.dev/dl/?mode=json&include=all 。


### willemt Raft：真实三节点库与双 leader 反例

先构建NP=3 tracer，再运行：

```sh
python3 examples/cases/raft_run.py --tracer ./tracer --depth 3 --timeout 240
python3 tests/raft_case_runner_test.py
```

runner复制现有库输入到独立build目录，仅将一次`switch(r->vote_granted)`改为
固定grant分支。原生对照证明：C投A并拒B，正常版B保持candidate；注错版B
误计拒票成为同任期第二个leader。模型中受控执行选举前缀，再由DFS发现该
违例；full/pages均检查精确诊断、任期、角色和实际拒票消息，三次重放同一边。

健康版三节点提交两条不同命令；保存状态中的命令内容由例子侧checker检查。
仅在确认时钟调用时推进leader时间，DFS前要求正预算。本轮两后端从预算1
出发各42次访问/27个unique状态；搜索仍受深度3及错误预算约束，不是Raft
一般安全证明。提交后的状态边三次恢复一致。日志payload由owned driver深拷贝
并在pop释放，无Raft日志压缩；DetSim的full/pages快照均实际使用。


### Rust std 与 CPython：真实线程生产/消费协议

详见 [concurrency/README.md](concurrency/README.md)。Rust使用std::thread/mpsc，
Python使用threading.Condition/list；保持实际GC/调度设置。两种注错分别遗漏或
重复任务1，worker输出实际消费的任务ID，公开ABI检查器要求精确一次、已创建
worker及退出0，不把运行时futex等待视为死锁。

full/pages共12项均完成原生对照、实际搜索和终点/违例边3x身份一致重放：
Rust健康45状态，遗漏/重复各41；Python健康410、遗漏390、重复392。
有界现有actor策略不等于穷尽共享内存交织。可选Queue实现仍保留；其_queue
动态扩展触发SIGBUS-backed映射拒绝，两个后端非绿色，未删除失败证据。
驱动仅在请求矩阵完整、唯一、全部通过时报告成功；七项负向回归覆盖原生超时、
Session创建异常和退出码，避免前几项成功掩盖后续缺行。
