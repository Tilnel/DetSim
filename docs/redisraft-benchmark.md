# Redis Raft 示例效率实测

后续 `experiment/page-snapshots` 分支的页格式、页级恢复及完整探索对照见
[页快照实现与测量](page-snapshots.md)，没有将下面的soft-dirty实验启为默认。

2026-10-14，生产代码基于 `b6cd591`；本次只新增测量驱动，不修改生产执行逻辑。

## 测量对象与方法

对象是仓库 `examples/redisraft/raft/raft`：Redis 维护的 Raft C 库加三节点
UDP 驱动，**不是完整 RedisRaft 模块或 Redis 命令吞吐测试**。
使用现成静态、带调试信息的二进制，没有重编译或改动子模块。
二进制 SHA256：`4e2085b6fa5497fc5c371775ea961c05880ff8f24833eb4e0ae9462fc27dbf26`。
子模块 HEAD 为 `7540ef5ad7458770432435643f0122a58dfa781f`，但不能据此推定
已有二进制准确对应该源码提交。

- 主机：AMD Ryzen 5 2600；tracer 为 GCC14 release、NP=3。
- 开启现有超时选择插件和 Raft 检查插件，插件从现有源码编译到 build 目录。
- 三节点均观察到单线程。C locale，不改 ASLR，每次使用独立目录和进程组。
- 先轮流推进节点0/1/2，共100轮、300次 syscall 状态转换。
  只有节点0在预计的 gettimeofday 边界选择超时；网络接收选择0。
- 从该检查点运行有限深度 DFS。使用仅供基准的 srand interposer，令 tracer
  搜索种子为1；现成 tracee 是静态程序，不加载该 interposer。
- 两档 L1/L2：32/64 MiB、256/512 MiB，预取窗口0，其余存储默认。
  每档每个深度各3次，下面报告中位数。总运行超时60秒，地址空间限3 GiB。
- 记录完整 commands/config/transcript、退出码、进程 VmHWM、墙钟/CPU时间、
  DFS计数和存储统计。最后正常退出并同步磁盘。

固定预热后，所有正式样本均为：节点0 leader，节点1/2 follower，term=1；
commit_idx 和 last_applied_idx 为 `[6,5,5]`，snapshot_last_idx 为 `[4,4,4]`。
因此确实经过选举、日志复制、应用和 Raft snapshot，不是空接收循环。
已有检查插件未报错，tracer 正常退出。剩余 error_bound=3。

## 结果

### 深度8，短搜索

两档每次均报告441次状态访问、223个新增状态。计数沿用当前 DFS 输出，
不将其解释为完整模型的状态总数。

| 指标 | L1/L2 32/64 MiB | L1/L2 256/512 MiB |
| --- | ---: | ---: |
| 初始提示符 | 39 ms | 26 ms |
| 300次手动转换 | 1.440 s | 1.530 s |
| 手动转换速度 | 208次/s | 196次/s |
| DFS含历史输出 | 1.998 s | 1.745 s |
| DFS报告速度 | 221次/s | 253次/s |
| 到搜索完成消息 | 1.501 s | 1.501 s |
| 完成后历史输出 | 0.497 s | 0.244 s |
| tracer峰值RSS | 309 MiB | 821 MiB |

短搜索的1.501秒受到监控线程500ms初始等待及1秒轮询/join影响，不能当成纯搜索
CPU耗时。增加缓存主要减少该次历史输出阶段的冷读，不能直接宣称核心搜索快14%。

### 深度12，较长搜索

两档每次均报告3,433次状态访问、1,776个新增状态；保存请求数、新快照数及
检查点协议状态也一致。

| 指标 | L1/L2 32/64 MiB | L1/L2 256/512 MiB |
| --- | ---: | ---: |
| DFS墙钟（含输出） | 14.019 s | 9.877 s |
| DFS访问速度 | 245次/s | 348次/s |
| 到搜索完成消息 | 13.505 s | 9.503 s |
| 保存区间累计 | 7.676 s | 6.269 s |
| 恢复区间累计 | 2.392 s | 2.134 s |
| tracer峰值RSS | 326 MiB | 1256 MiB |
| DFS统计冷读次数（含检查/历史） | 518 | 0 |
| 全程存储文件大小 | 292 MiB | 292 MiB |

该规模下大缓存将整体访问速度提高约42%，但峰值RSS接近小缓存的4倍。
保存+恢复区间占墙钟约72%–85%，并非单纯磁盘带宽限制。


## 开销判断与限制

- 保存和恢复是主要可见开销。计时来自 state_transition 的捕获/保存与恢复区间，
  不等同于完整 CPU profile；异步压缩、检查插件、输出和监控另有成本。
- 深度12中，11,127次子快照 save 调用只有403个新快照，去重率96.4%。
  大量重复状态仍经历捕获、序列化和 hash/字节比较。应优先查明未变化节点为什么
  仍进入完整捕获，以及不变页能否安全复用，而不是先扩大缓存。
- 大量检查表达式访问同一个缓存快照，按请求计数的命中率接近100%，不能据此
  认定冷读无成本。需要结合实际冷读次数、字节量和恢复时间判断。
- 增加缓存有明显内存代价，缓存容量不是 RSS 上限：调用者缓冲区、pending、
  数据结构与分配器仍占内存。所有 RSS 数字仅指 tracer，不含三个 tracee。
- 这是有限深度、受错误预算和现有 Raft 剪枝约束的当前搜索实现；FIFO/线程/
  环境结果覆盖问题仍见 correctness-backlog。没有跑无限 BFS，也不声称穷尽。
- 单步速度包含 Python/CLI 往返。DFS墙钟包含监控和历史输出；不是 Redis QPS，
  也未与原生 RedisRaft 吞吐比较。

首次试跑发现：每条 syscall 都传 `si 1` 会消耗网络重排预算，使 DFS 只报告2个
状态。该结果排除出正式性能样本。未固定随机种子的试跑也因遍历顺序不同产生
不同访问数量，没有用于缓存对比。固定种子后每档计数完全一致。

## 复现

```sh
make benchmark-redisraft CC=/usr/bin/gcc CXX=/usr/bin/g++
make benchmark-redisraft CC=/usr/bin/gcc CXX=/usr/bin/g++ \
  REDISRAFT_BENCH_ARGS='--depth 12 --hot-mb 256 --warm-mb 512'
```

默认 NP=3、开启两个插件、固定种子1、300次手动转换后 DFS深度8。
也可以直接运行 `tests/redisraft_benchmark.py --help` 查看参数；直接运行时
需传入对应的 `--choose`、`--check`、`--seed-shim` 以复现正式配置。

产物在 `build/redisraft-benchmark/run-*/`，正式汇总为 `final-summary.json`
（深度8）和 `depth12-summary.json`；构建目录不纳入 Git。
`make check`、Python语法检查和上述基准入口均已验证。

## 保守捕获优化后的对照

后续优化保留所有节点的完整内存、映射、brk、GP/XSTATE捕获；没有假设停止的
节点不会被异步或跨进程写入。删除原来因 brk=0 持续失效的元数据比较以及
整份旧系统状态深拷贝，显式源快照仅作为存储去重提示。pending/L1中已有
候选且完整字节相等时跳过一次hash扫描；冷缓存/不匹配继续普通保存。

使用优化前保存的NP3二进制（`e3db485`）与新二进制交替跑固定种子1、深度12、
同样插件和300步预热，每档各3次，中位数如下。与前一节不同测量时段的绝对
时长不要混用；这里是本轮交替采样结果。

| 指标 | 32/64 MiB，前→后 | 256/512 MiB，前→后 |
| --- | ---: | ---: |
| 保存区间累计 | 7.872→7.202 s（-8.5%） | 6.360→5.879 s（-7.6%） |
| DFS含输出 | 14.028→13.029 s | 10.889→9.881 s |
| DFS报告速度 | 245→263次/s | 315→347次/s |
| 300次手动转换 | 1.483→1.403 s | 1.585→1.589 s |
| tracer峰值RSS | 314→316 MiB | 1249→1253 MiB |

整体墙钟仍受监控线程一秒轮询取整影响，不能将整整一秒的下降全部归于hash
优化；更可靠的证据是保存区间累计耗时下降。手动大缓存路径没有明显改善，
内存也没有明显减少。

所有样本仍报告3,433次访问、1,776个新增状态、11,127次子快照保存请求、
403个新快照；两档冷读分别仍为518和0。新reference计数分别为6,139/6,140，
表示这些已完整捕获的快照省去重复hash。现有Raft检查和检查点协议结果一致。

`make check-capture` 另用同一对真实ptrace进程逐字节比较优化/普通路径，包含
分支恢复、外部修改非活动节点内存、socket/时间变化及极小缓存。
`DETSIM_CAPTURE_MODE=full` 可禁用提示；`verify` 会对提示路径额外执行普通保存
并检查返回hash。结果保留在 `build/capture-optimization/results.json`。

## 全链路 CPU profile（`8d58059`）

结论：**仍有优化空间，当前主要限制是每条边对全部节点做完整捕获和恢复，
尚未达到性能上限。** 下述测量覆盖正常 CLI 的 DFS、属性检查、后台工作、
监控及结束后的历史输出；不把存储微基准当作探索性能。

固定上述工作负载、seed=1、预热300步、DFS深度12，各缓存档不采样运行3次，
中位数如下。每次仍为3,433次访问、1,776个新增状态；当前有限深度DFS的visited
和Raft剪枝仍有覆盖限制，这些计数仅用于控制本次工作量。

| 全链路指标 | L1/L2 32/64 MiB | L1/L2 256/512 MiB |
| --- | ---: | ---: |
| DFS墙钟，含历史 | 13.029 s | 9.889 s |
| tracer全部线程CPU时间 | 15.390 s | 12.100 s |
| 捕获/保存计时区间 | 7.373 s | 6.066 s |
| 恢复计时区间 | 2.469 s | 2.217 s |
| 完成消息后的历史输出 | 0.521 s | 0.378 s |

另一次不采样运行新增了主线程及三个tracee的CPU计数：大缓存墙钟9.883s，tracer
主线程8.640s，tracer全部线程12.790s，三个tracee合计0.300s；小缓存分别为
14.035s、12.430s、15.410s、0.320s。这是单次交叉检查，不与上表中位数混算。
tracee计数包括注入brk等辅助syscall；下面的“推进”比例只描述tracer调度/模拟
CPU，不能解释为目标程序自身只消耗该比例的端到端时间。墙钟还包括等待与报告。

使用可工作的 `/usr/lib/linux-tools-6.8.0-138/perf`，`sudo -n` 仅用于获得采样权限；
没有改sysctl或ASLR。默认 `/usr/bin/perf` 包装器找不到当前内核工具，另一个
6.8.12二进制缺libperl，均未用作正式采样。`cpu-clock` 199Hz、DWARF栈32KiB，
在DFS前附加到tracer及其已有线程；三个tracee早已创建，不在采样对象内。
按单调时钟过滤掉perf启动/停止阶段。主线程调用栈按已识别阶段固定优先级互斥归类；
不能完整回溯的栈保留在“其他”，不强行归到某一阶段。
捕获分类识别的是save_full_state_to_state_store主体；它之前的brk注入等工作进入
“其他”，所以该表不是完整捕获计时区间的精确CPU分解。

| 主探索线程CPU样本占比（约数） | 32/64 MiB | 256/512 MiB |
| --- | ---: | ---: |
| 完整捕获，含去重/保存 | 50% | 58% |
| 恢复 | 18% | 23% |
| 属性检查 | 15% | 9% |
| 推进目标程序/模拟syscall | 1% | 2% |
| 历史输出 | 2% | 2% |
| DFS重载及其他未归类工作 | 13% | 6% |

两档正式采样分别2,978/2,175个样本，其中主线程2,465/1,651个；这属于粗粒度定位，
小函数的百分比不宜当成精确计时。大缓存采样9.941s、小缓存14.077s；后者相对
不采样中位数多约1秒，受采样开销及monitor一秒join量化共同影响。采样运行也验证
相同访问数、快照请求数与协议检查结果。一次与strace同时运行的试样未用于此表。

后台worker占所有tracer线程CPU样本约16%/23%，主要是压缩、解压和落盘；这些
工作与主线程重叠，不能将CPU百分比直接加到墙钟，或承诺删掉它们就节省同样时间。
大缓存仍有约46%的全部CPU样本在内核，主要是process_vm读和页访问，不是磁盘等待。
大缓存没有冷读；小缓存519次冷读，与历史样本518的差异来自当次缓存淘汰时序。

### 可榨空间及优先级

1. **删除多余清零和内存分配。** 大缓存主线程约6.4%的样本落在捕获路径清零；
   `state.cpp` 的 `vector<uint8_t>(total_size)` 先清零整份约1.25MB缓冲区，紧接着
   process_vm_readv又覆盖内存部分。可复用缓冲区或仅初始化格式头/表，必须证明
   所有发布字节均已写入，保持padding/identity确定性并继续严格拒绝短读。
   这是一项局部优化；即使完全消除该6.4%主线程CPU，Amdahl上限也只有约1.07倍，
   不是承诺墙钟能提升7%。
2. **减少重复元数据读取和ptrace往返。** 约3,709次transition产生11,127次子快照
   捕获，累计构造约13.9GB快照字节；每次还注入brk、读maps、枚举线程、读GP/XSTATE。
   对主线程做strace计数，并减去相同预热但depth=0运行，探索及历史大致额外产生
   175,958次ptrace、38,230次wait4、49,582次process_vm_readv、11,419次process_vm_writev，
   以及39,370次getdents64、39,372次openat。这里只使用计数：strace明显扰动速度，
   它给出的时间比例不代表正常运行。优先复用同次捕获/恢复已取得的主线程寄存器和
   线程列表；缓存brk/maps则需要明确的失效和外部变化契约，不能直接永远复用。
3. **让属性读取持有已验证的不可变snapshot view。** 本次有662,872次load请求，
   相当于每条transition约179次。检查一个long目前也要查缓存、验证区域、malloc、
   扫描区域。每轮检查固定持有各节点快照、验证一次、读到调用者缓冲区可降低成本。
   小缓存中检查约占主线程15%，部分是读取导致解压。AST解析缓存已经存在；
   Raft checker会修改raft_states，不能直接按hash缓存check结果而忽略副作用。
4. **减少恢复的重复全区域传输。** 当前先读完整live区域、memcmp；有任何差异就
   写整区。可保持完整读取但逐页比较、合并不同页写回，尤其适合大heap小修改。
   本次writev相关成本较小，收益需另测，不能仅凭结构就优先于捕获读取。
5. **消除命令尾部等待。** monitor睡500ms后每秒轮询，stop直接join。
   可唤醒等待能消除最多约1秒尾部延迟；历史按需输出可再少约0.4–0.5秒。
   两项改善命令响应，不能当作持续搜索吞吐提升。
6. **大幅提升需要减少工作量。** 同样边数下，用可靠dirty追踪、页级共享状态避免
   完整内存反复搬运；或用经证明的独立性/POR减少边数。当前每条边只推进一个节点，
   却完整捕获所有节点，成本随边数×节点数×地址空间增长。停止的节点仍可能被
   跨进程或异步kernel写入；目前默认syscall passthrough未形成封闭支持契约，
   因而不能仅因“没调度这个节点”就跳过它。直接合并syscall会改变属性观察点及
   交织覆盖，必须先证明atom和约简规则。

DFS把刚捕获的状态只以hash交给子帧，之后又从存储反序列化；直接传递已捕获状态
也有可删的重复工作，但本次可识别CPU占比低，优先级低于前述热点。访问数相同
只是本次性能对照依据；后续涉及剪枝/atom的优化必须检验successor及属性覆盖。

### 复现profile

```sh
python3 tests/redisraft_benchmark.py \
  --choose build/release/np3/tests/redisraft_choose.so \
  --check build/release/np3/tests/redisraft_check.so \
  --seed-shim build/release/np3/tests/redisraft_seed.so \
  --depth 12 --hot-mb 256 --warm-mb 512 \
  --perf /usr/lib/linux-tools-6.8.0-138/perf --perf-sudo \
  --output build/exploration-profile/hot
# 使用上一命令输出的workdir替换下面RUN：
sudo -n /usr/lib/linux-tools-6.8.0-138/perf script \
  -i RUN/perf.data -F comm,pid,tid,time,event,ip,sym,dso > RUN/stacks.txt
python3 tests/redisraft_profile_summary.py RUN/result.json RUN/stacks.txt
```

采样产物和未采样基线保留在 `build/exploration-profile/`；正式大缓存
`hot/run-97j2m9t2`、小缓存 `cold/run-qt4fq_5d`。新驱动另记录DFS单调时钟窗口；
两份汇总是 `hot-summary.json`、`cold-summary.json`，syscall差分是
`syscall-count-delta.json`。本轮没有修改产品捕获、恢复或搜索语义。

## Soft-dirty 捕获实验（2026-10-15）

**在当前完整快照架构和该Raft负载上，没有测到全链路收益，因此默认关闭。**
跨进程读取量减少约90%，但捕获的小幅改善被恢复和追踪开销抵消。此结论不等于
soft-dirty在所有负载上无用，也不支持将它直接作为通用正确性保证。

实验保留版本5格式、完整缓冲区、hash/去重、寄存器捕获、原有恢复和搜索。
`DETSIM_SOFT_DIRTY_EXPERIMENT=verify` 重建原始内存后再独立全读、逐页比较，
发现漏页立即失败；`delta` 省去独立全读。未设置变量时走原完整读取路径，不构造
追踪器、不扫描pagemap、不写clear_refs。模式在首次调用固定，不支持运行时切换。

每个live PID保留原始内存基线；同布局时读pagemap bit55，将脏页及nonpresent页
合并为区间读取，其他字节从基线复制。布局变化全读回退。读取完成后写
`/proc/PID/clear_refs=4`；恢复写入继续计入下一次捕获，不能随便在恢复后清除标记。
所有基线及oracle均在rseq归一化之前处理。

### 正确性实验

- 独立ptrace fixture：用户写入、内核read copyout、外部process_vm_writev、只读页
  PTRACE_POKEDATA、MADV_DONTNEED、同地址unmap/remap、恢复式写入、跨页部分范围、
  多个不连续输出buffer、超过256页的分批扫描，以及布局变化全读回退。
- 故意由外部清除epoch：完整oracle确实拒绝漏掉的变化页，不是永远报成功。
- 现有NP2 capture gate：默认、verify、delta分别运行普通/极小缓存两种配置，
  覆盖分支恢复、非活动节点外部修改、socket/time变化。verify包含独立完整读取。
- Raft深度12的v1/v2 verify均完成11,127次捕获，`missed_pages=0`，访问数
  3,433、新增状态1,776、新子快照403，协议检查结果一致。

这仅验证已执行的路径，不能证明所有变化来源都被追踪。尤其baseline读取和
clear_refs之间仍有窗口：异步writer在该窗口写入，会被清位却未进入基线。
固定且停止的线程不排除AIO/io_uring、pinned页、设备及外部backing修改。
还要求独占该PID的clear_refs epoch、固定进程生命周期；PID复用、exec、THP/swap
及真实异步writer的通用覆盖未验证。实验不改变项目现有支持边界。

### 全链路结果

同一GCC14 NP3二进制，固定seed=1、300步预热、DFS12、相同两个插件。
每档full/delta各3次，串行交替，下一轮反转顺序。以下为第二版中位数：

| 指标 | 32/64 MiB full→delta | 256/512 MiB full→delta |
| --- | ---: | ---: |
| DFS墙钟，含历史 | 14.032→14.037 s | 9.890→9.881 s |
| tracer全部线程CPU | 15.540→15.560 s | 11.830→12.000 s |
| tracer主线程CPU | 12.450→12.480 s | 8.640→8.850 s |
| 三个tracee合计CPU | 0.350→0.380 s | 0.340→0.370 s |
| 捕获/保存区间 | 7.511→7.259 s | 5.769→5.624 s |
| 恢复区间 | 2.546→2.761 s | 2.175→2.496 s |
| tracer峰值RSS | 317→336 MiB | 1254→1259 MiB |

全部样本计数及检查点一致。毫秒级墙钟差异不构成收益，且monitor尾部仍有一秒
量化；CPU与区间计时也没有支持稳定的整体加速。小缓存冷读为518–520，淘汰时序
有变化；大缓存均无冷读。

第二版每次DFS本可全读13.810GB，实际读约1.351GB（减少90.2%）；扫描约337万
页，约33万页需要读取。扫描加基线→输出复制约1.33–1.43s，clear_refs约0.25s。
`bytes_read`不含verify额外oracle；正式性能样本均关闭oracle。第二版只把脏区间
复制回基线，该方向复制量也约1.351GB；仍保留另一方向的整份基线复制。

第一版在每次捕获后整份复制回基线，大缓存墙钟9.890→10.881s、小缓存
13.025→15.034s，明显更差。删掉该复制后才回到整体接近持平，故不能把读取字节
减少直接当成加速。两版不同测量时段的绝对时长不直接混比。

捕获阶段仍清零、复制、拼装及比较/hash完整快照；dirty追踪只减少了其中的远程
读取。恢复仍完整读取并在区域有差异时整区写回。清位撤掉页表可写位，给随后的
写入增加处理成本，与恢复区间变慢相符；尚未单独隔离每种写入成本，不能全部
归因于某一种缺页。继续开发需要同时考虑页级恢复和避免完整缓冲区搬运的表示，
还要先解决上述追踪窗口与支持契约。当前不建议仅凭此实验推广增量捕获。

### 复现与产物

```sh
make NP=3 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4 tracer \
  build/release/np3/tests/redisraft_choose.so \
  build/release/np3/tests/redisraft_check.so \
  build/release/np3/tests/redisraft_seed.so
make check-soft-dirty CC=/usr/bin/gcc CXX=/usr/bin/g++
DETSIM_SOFT_DIRTY_EXPERIMENT=verify make NP=2 check-capture \
  CC=/usr/bin/gcc CXX=/usr/bin/g++
python3 tests/soft_dirty_benchmark.py --repeats 3 --depth 12
```

Raft verify可在前述benchmark命令前加`DETSIM_SOFT_DIRTY_EXPERIMENT=verify`。
`soft_dirty_benchmark.py`按模式清理环境、检查工作量计数并保存逐次日志、原始结果
和中位数，不改变默认后端。运行前确保root tracer是NP3。
本次产物保留在`build/soft-dirty/results-v1.json`、`results-v2.json`、
`summary-v2.json`及对应run目录。新增runner另以深度8、每档每模式1次验证入口。
产品构建、`make check`、NP1原生scenario/state-io/mappings及上述capture gates通过。
