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

## 已落地的两个问题

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
| Rust std | rustc1.51.0 | 通过 | 仍阻塞于 `sigaltstack`，syscall131 |
| Go | gccgo14.2（非标准 gc） | 通过 | pages 推进到 `sigaltstack`；full 捕获发生 `bad_alloc` |
| Python | CPython3.13.5，`-I -S` | 通过 | 已越过 `readlink`；缺少映射的标准库，退出1，验收失败 |
| Java | OpenJDK21.0.12.1 | 通过 | 已越过 `readlink`，下一阻塞为 `access`，syscall21 |

这些版本是本机实测，不代表整个语言、其他版本或所有库已受支持。Java 保留正常 JVM/JIT，只显式限制 heap/code cache/class space；没有把 JVM 私有结构补丁放进框架。标准 gc Go 的单独准入目标尚待可用工具链；不能用 gccgo 结果代替它。

本次案例推动了三个通用修复：动态 argv（原数组在5参数写终止符时越界，9参数直接拒绝）、匿名零长度 mmap 的正常 `EINVAL` 路径、`poll(timeout=0)`。文件 mmap 的 `EBADF` 优先级和 huge-page 不支持边界仍保留；阻塞 poll 仍明确拒绝。

后续这一批补了 `sched_getaffinity` 固定虚拟 CPU0 查询，以及 `readlink/readlinkat`：链接目标复用 VFS 节点内容保存，初始化捕获 executable 路径链和 `/proc/self/exe`，探索期间不查询宿主路径。普通链接遍历仍明确拒绝；未支持完整 procfs 或 CPU 拓扑。真实 syscall 的 full/pages 三次恢复重放另有回归。内核模型版本升为 v12，快照布局不变。

为正确描述 executable 文件，当前捕获其完整字节，会增加 VFS 复制与哈希成本。12 组小案例单次总耗时旧版12.43秒、新版12.19秒（含编译），小案例未显示明显退化，但 Redis SET/GET 场景从先前7.691秒变为26.012秒，约3.38倍，属于实际性能回退。原因是现有 VFS 用独占 vector 保存文件，静态 executable 字节也随每个状态复制、哈希和序列化；后续应统一改为不可变内容块和分支写覆盖。不能把这称为 readlink 的必然成本。gccgo 的内存失败也单独保留，不以另一次 pages 的进展掩盖。

## 接下来的推进顺序

1. 用上述未通过的 probe 驱动 `sigaltstack`、`access`、只读运行环境文件及大规模保留映射捕获的支持。每修一处先让原案例继续，再记录下一个阻塞；不要靠放宽未知 syscall passthrough 或跳过初始化来变绿。
2. 为各语言加入正常/注错小协议，保持相同的证据等级，而不仅是 Hello World。
3. 接入 willemt Raft 与现有 RedisRaft **库驱动**，分别定义 leader/term/commit 性质及可解释的复制构建 mutation。已有 `benchmark-redisraft` 不是完整 Redis+RedisRaft module 的证明。
4. 再推进完整 RedisRaft、真实多线程/多进程系统。只有具体案例要求时再扩展 actor/timeout/outcome 枚举和共享内存切换边界。

候选目标与状态见 [catalog.json](catalog.json)。已知限制必须保持为 unsupported、bounded、timeout 或 failed，不能统计成已验证安全。`case_runner_integration.py` 专门验证深度不足和缺失工具链不会变绿。
