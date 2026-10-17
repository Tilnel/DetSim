# 页快照分支：实现与全链路测量

分支 `experiment/page-snapshots`。页捕获实现基于 `08ed190`；后续虚拟 I/O 修复
扩充了元数据，格式先升级到7/8；通用框架改造后升级到9/10，进程内核状态闭合后升级到11/12，拒绝旧5/6/7/8/9/10，不迁移或删除旧存储。默认仍生成v17完整快照，
`DETSIM_SNAPSHOT_BACKEND=pages` 启用新的v18页对象格式。
**主要收益来自表示和去重粒度变化；启用页格式不要求依赖脏页标记。**

## 行为与持久化

- v18根对象是小manifest：canonical映射、Cereal tracee元数据、GP/XSTATE及
  按区域/地址排列的页引用。每份页内容用原有StateStore独立保存，不引入父快照链。
- 页后端默认完整读取支持的私有可读区域，用每个live PID的工作buffer避免
  每次分配/清零整份内存。先做现有rseq归一化，再逐页与上一次不可变页比较；
  **字节相等**才复用已接受的页key。布局/内容变化不会因提示而省略读取。
- 新增SnapshotView共同读取v17/v18；表达式仅加载涉及的页，直接读到栈上的long。
  maps/元数据检查、范围读取、恢复不再各自解释格式。去掉旧代码中的重复解析分支。
- v17和v18都保持完整live区域读取，逐页与保存内容比较，只将不同页加入writev。
  批次持有页owner，淘汰不能使指针悬空；权限、rseq和寄存器恢复保留原契约。
- 缺页、长度错误、错误内容key及错误manifest范围显式失败。内容key的验证复用
  StateStore已有的保存/加载校验，不对可信不可变buffer重复hash。
- lifecycle generation在init/shutdown及首次存储失败变化。暖解析/页缓存也拒绝
  已关闭或poisoned store；初次自动初始化不能混入另一个存储生命周期的buffer。
- 页对象先接受、manifest后接受；冷进程完整读取以completion barrier为界。
  worker实际完成顺序可能不同，没有新增跨sstate与snapshot store的事务保证。

v18在格式内身份确定，**v17/v18 hash不相等**。对照比较解码后的元数据、maps、
GP/XSTATE、区域属性和全部地址字节，不能用跨格式hash相等作为验收。
物理TID、跨运行canonicalization和其他原有正确性缺口没有在本次关闭。

## 内存与支持范围

解析缓存按payload估算限制为4MiB、最多256项；每个view最多缓存一个页。
恢复只持有当前256个待写页owner。每个捕获PID另保留一份完整工作内存和一组
上次页owner作为精确比较提示；这些、容器/allocator、索引及调用者owner都在
StateStore L1/L2预算之外。本阶段面向固定live tracee集合，未支持PID复用或exec
生命周期管理。预算不是总RSS上限。

独立 `DETSIM_SOFT_DIRTY_EXPERIMENT=delta|verify` 可与页格式组合，但仍是实验：
baseline读取到clear_refs之间的异步写入窗口未解决，pinned/AIO/设备/外部backing
变化没有通用覆盖证明。下面的主要收益使用完整读取路径。

## 验证

- `check-page-snapshot`：flat/pages、跨页/相邻区域、cold/tiny缓存、缺页、错误长度和
  内容key、错误manifest覆盖、持有owner、关闭/reopen、暖缓存sync失败拒绝。
  另以fork+exec后的全新进程在writer barrier后读取全部页，不借用进程内缓存。
- NP2 `check-capture`：同一组真实ptrace进程跨格式逐字段/逐字节对照，包含
  恢复、外部修改非活动节点、socket/time变化、普通/极小缓存。
- mapping fixture：RO/split/merge/unmapped等原有case之外，1MiB区域仅改首末页，
  完整比对恢复结果，断言实际写量小于整区。保持严格短I/O拒绝。
- 默认与verify-pages分别通过NP1原生scenario、mapping、XSTATE、futex snapshot；
  NP2 Redis精确SET/GET、正常client退出均通过。
- Raft verify-pages完成深度12全部11,127次捕获，每次独立全读对照；另与soft-dirty
  RAW oracle组合复核。有限通过不证明完整模型/所有Linux变化来源。

## 全链路结果

同主机GCC14 NP3、相同Raft二进制及两个插件、seed=1、300步预热、DFS12。
四种模式、两档缓存各3次，串行交替、下一轮逆序。每次均为3,433次访问、
1,776个新增状态，检查点协议值和检查插件结果一致。以下中位数包含DFS历史输出。

| 指标 | 原版32/64 MiB | 页格式32/64 MiB | 原版256/512 MiB | 页格式256/512 MiB |
| --- | ---: | ---: | ---: | ---: |
| DFS墙钟 | 13.027 s | 7.514 s | 9.893 s | 7.513 s |
| tracer全部线程CPU | 15.420 s | 6.840 s | 11.840 s | 6.860 s |
| tracer主线程CPU | 12.350 s | 6.730 s | 8.700 s | 6.740 s |
| 捕获/保存区间 | 7.348 s | 4.117 s | 5.716 s | 4.134 s |
| 恢复区间 | 2.479 s | 2.017 s | 2.174 s | 1.984 s |
| tracer峰值RSS | 338 MiB | 44 MiB | 1250 MiB | 45 MiB |
| 全程存储产物 | 306.4 MB | 3.38 MB | 306.4 MB | 3.40 MB |
| 300次手动转换 | 1.489 s | 0.550 s | 1.649 s | 0.547 s |

墙钟约减少42%/24%，全部线程CPU约减少56%/42%。monitor仍按秒join，精确墙钟
下降不能全部归因于计算；CPU、区间计时及存储/RSS均支持明显收益。
前一轮原型重复校验可信buffer的hash导致v5回退路径变慢，已删除该重复计算。
最终分支v5路径小/大缓存墙钟13.038/9.880s，全部线程CPU14.920/11.550s，
与原版接近；不能把旧原型的绝对时间与本轮混用。

页格式DFS仍完整读取13.810GB；约337万页比较中复用约335万页引用，只需提交
约1.4万次变化页请求，再提交11,127次小manifest。`save_calls/new`现在统计
**页对象和manifest**，不能与原版“整份子快照”的对象数直接比较。
恢复完整读约5.31GB，实际写约49–51MB。相同代码页不再在每个新状态重复压缩、
保存、解压；后台CPU从数秒降到约0.1秒，是全链路改善的一部分。

组合soft-dirty后目标读取降至约0.11GB（较完整页捕获少99.2%），但扫描+基线复制
约1.5–1.6s，清位约0.24s，恢复略慢。小/大缓存DFS均仍约7.514s，全部线程CPU
6.790/6.900s，对照完整页路径6.840/6.860s，没有稳定净收益。
因此没有将脏页追踪启为默认，也没有把读取量下降宣称为探索加速。

这仍是当前有限深度、有错误预算及Raft剪枝的实现；相同计数控制性能工作量，
不证明successor覆盖完整，也不是完整RedisRaft模块吞吐。
Redis单次精确SET/GET交叉检查：分支v5约9.51s、带额外全读验证的页后端约4.99s；
仅作为第二个工作负载的功能证据，不作为重复采样的性能结论。

## 复现

```sh
make NP=3 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4 tracer \
  build/release/np3/tests/redisraft_choose.so \
  build/release/np3/tests/redisraft_check.so \
  build/release/np3/tests/redisraft_seed.so
make check-page-snapshot CC=/usr/bin/gcc CXX=/usr/bin/g++
DETSIM_SNAPSHOT_BACKEND=verify-pages make NP=2 check-capture \
  CC=/usr/bin/gcc CXX=/usr/bin/g++
# 若前一步另外构建了NP2 tracer，先重新选择NP3：
make NP=3 CC=/usr/bin/gcc CXX=/usr/bin/g++ tracer
python3 tests/page_snapshot_benchmark.py --repeats 3 --depth 12
```

`--before-tracer PATH` 追加原版NP3对照；`--soft-dirty` 追加显式实验组合。
`DETSIM_SNAPSHOT_BACKEND=verify-pages` 用于独立全读对照，不用于正式性能样本。
原始产物保留在`build/page-snapshots/results-final.json`、`summary-final.json`及run
目录；原版二进制是改造前保存的NP3 tracer。runner另以深度8验证所有模式入口。

本页性能数字来自 I/O 修复前的 v5/v6 实现（commit `1c4e8aa`），不能直接作为 v7/v8 的测量结果。

当前两种格式通过同一个 SnapshotView 读取；FlatMemory/PagedMemory 仅负责提供不可变 chunk。应用投影与 monitor 状态不混入机器快照，后者保存在版本化系统状态中。历史性能数据的格式和语义以各节标注为准。


## 已验证的零匿名保护区恢复（2026-10-17）

恢复默认对一类严格证明的区域使用 `MADV_DONTNEED`，替代临时解除保护后逐页
写入零字节。它不是脏页追踪：捕获仍全量读取支持的内存，包括保护区隐藏字节。
没有新增语义字段、改变状态身份或升级flat37/pages38格式。

区域必须保存为PROT_NONE、起止4KiB对齐，保存映射与当前真实映射都完整覆盖该
范围，且全部为无名字、inode0、device0的私有匿名映射。目标快照的虚拟文件映射
覆盖表及来源表任一重叠都排除，因为匿名native VMA可能承载模拟文件内容。
不能只凭PROT_NONE或某个零页hash判断。优化通过共同SnapshotView逐页读取并
验证整个保存区域，再比较实际字节；非零区域继续走原路径。

确认全零后先flush此前排队写入，再对该区域执行native MADV_DONTNEED并恢复
最终PROT_NONE。该系统调用对私有匿名映射提供按需填零语义，参见
[Linux man-pages](https://man7.org/linux/man-pages/man2/madvise.2.html)。返回非零时
强制完整写回保存内容，包含部分discard后返回错误的情况；意外ptrace/stop异常
继续向上传播，沿用已有dirty标记契约，不把恢复失败伪装成成功。验证是逐区域的，
不是整次恢复事务：后续区域失败时，先前区域可能已经改变。

`DETSIM_ZERO_GUARD_RESTORE=write` 强制旧写回路径供对照；默认或显式`discard`
启用优化，其他值在恢复入口明确拒绝。物理驻留、mincore/mlock等观察不在当前
闭合执行模型内；不因这项优化声明支持它们。性能计数器新增`zero_guard_bytes`、
`zero_guard_regions`与`zero_guard_fallbacks`，分别记录成功discard字节、区域数及
全零候选native调用失败后回退次数，均不进入状态身份。

验证包含full/pages/verify-pages各普通/极小缓存六组：原生保护区discard对照、
保存零/当前非零、可读live分支、保存非零末页、冷打开、3x同hash，独立文件来源
表排除、保存device/shared/named条件排除、真实live文件映射排除，以及负返回、
部分discard后失败、异常传播、缺失末页在discard前失败、未知模式拒绝。修改前
默认路径失败于“未调用discard”，修改后六组全部通过。
外层dirty标记的异常保持经过源码审查，本次没有新增该外层的专项回归。

### 合成有限探索的全链路测量

可重跑的 `tests/zero_guard_benchmark.py` 在已有two-debits有限程序上增加192MiB
全零私有匿名PROT_NONE映射，保留真实两次时间选择及独立输出检查。先验证四种
native输入，再用同一NP1二进制、每次fresh store，按交替顺序各后端跑3对write/
discard。12轮均18个访问/新增状态、正常退出及3次相同hash终边重放。
下面为3次中位数；仅是该合成fixture的描述性测量，不是Redis性能或统计显著性结论。

| 指标 | full write→discard | pages write→discard |
| --- | ---: | ---: |
| 搜索秒数 | 7.181656→6.904347（-3.86%） | 1.669046→1.322689（-20.75%） |
| 整体driver秒数 | 11.091773→10.558289（-4.81%） | 2.303499→1.736618（-24.61%） |
| 恢复时间ms | 1245→995 | 262→21 |
| 捕获/保存时间ms | 5451→5420 | 1400→1296 |
| GNU time最大RSS KiB | 630596→630572 | 245052→245004 |

整体driver时间含启动、搜索、重放和退出，不含编译/native前置检查。搜索期间
3次discard共576MiB，恢复写入从约576MiB降至约0.1MiB；这些计数在终点重放前
输出，不是整个driver的所有写入。最大RSS是GNU time命令指标，不能等同于所有
进程RSS之和；该指标基本不变。full仍需读取/构造、压缩与验证完整映像，捕获/
保存耗时几乎不变，因此省下大量恢复写入不代表整个探索能得到同等倍数收益。

最终产物：`build/zero-guard-final-benchmark/run-dbnqqtqm/results.json`；
独立审计：`build/zero-guard-audit/benchmark-audit.json`及`verify_benchmark.py`。
先前prototype的12轮与1MiB脚本入口4轮保留在其他目录，不混入上表中位数。

```sh
make NP=1 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4
python3 tests/zero_guard_benchmark.py --tracer ./tracer --size-mb 192 --repeats 3
make NP=1 check-mappings CC=/usr/bin/gcc CXX=/usr/bin/g++
```

另有NP1 state-I/O/page-snapshot、NP2 capture/identity/plugin、29个原生ptrace场景、
4组线程生命周期、8个输入case与4个pthread case回归通过，state.cpp的Clang静态
分析无诊断；生产NP1/2/4构建通过。12个case行共44个会话的48次原始重放另经独立
核查。没有把这些局部回归声称为全部运行时矩阵复验；完整RedisRaft四项timeout
尚未被这项测量解决，C03/C08/C25的一般缺口也没有据此关闭。
