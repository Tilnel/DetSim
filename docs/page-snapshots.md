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
