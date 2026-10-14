# 状态存储契约与实现

本页描述当前实现。快照是否包含完整程序状态仍由
[正确性 backlog](correctness-backlog.md) 跟踪；存储正确不等于模型检验完整。

## 三层职责

| 组件 | 负责什么 | 文件 |
| --- | --- | --- |
| `StateStore` | 不可变快照缓冲区、L1/L2、异步写入、预取、完成屏障 | `state_store.{h,cpp}` |
| `StateStorePacked` | 压缩记录、分段追加、索引日志、校验及同步 | `state_store_packed.{h,cpp}` |
| `SysStateStore` | 系统状态元数据的追加日志、索引与有界缓存 | `sysstate_store.{h,cpp}` |

源文件均在 `src/core/state/`。`state.cpp` 负责捕获、恢复和语义身份，
存储层负责保留给定字节。当前程序快照格式是 v5；旧快照明确拒绝。

## 保存、完成与失败

`StateStore::save` 计算完整 64 位 hash，复制为不可变缓冲区，接受写入后返回。
这时可以读取该状态，但尚不承诺磁盘持久性。重复 key 必须比较内容；不同内容
不能因为 hash 相同而被默默合并。零长度和超过 packed 格式大小上限的输入拒绝。

捕获层可传入源 hash 作为去重提示。存储层只查看已驻留的 pending/L1 不可变
缓冲区；长度和全部字节相等时可直接返回该 hash，省去一次重复 hash 扫描。
未命中或不相等时走原路径，不为提示额外读取磁盘。只读、失败和长度检查仍执行。
这不会省略任何进程内存、映射、brk 或 GP/XSTATE 捕获。`reference` 统计表示
通过该路径完成的保存数量；`save_calls` 仍统计所有调用。

诊断用 `DETSIM_CAPTURE_MODE=full` 禁用提示，`verify` 用普通保存路径再次检查
结果；两个选项不改变快照格式或搜索策略。`make check-capture` 用同一对真实
ptrace进程比较两条路径，包含非活动节点被修改及极小缓存下的完整字节对照。

pending 集合从接受写入到成功写完始终持有数据，包括已出队的任务。
`wait_for_completion()` 等待 pending 清空，再同步 packed 数据、索引日志和目录。
屏障完成前阻止新写入被接受；I/O 失败会抛出异常，不会假装写入完成。
屏障通过后，处于静止状态的写入者可以保持打开，另一个只读进程重新打开并读取。
这正是 C26 的回归条件，不再用 shutdown 绕过屏障。

`shutdown()` 停止接受新操作，排空已接受写入，回收线程并同步文件。
失败显式报告；析构函数不抛异常。tracer 的显式退出清理捕获错误、回收 tracee，
并以非零状态结束。关闭后再次使用必须显式 `init`；第一次使用允许默认初始化。

worker 的压缩、写入、预取失败以及完成屏障的同步失败会记录为错误状态。
后续操作拒绝继续，直到 shutdown/reopen。短 I/O 和 EINTR 在底层完整处理；
损坏、截断、错误 checksum、错误记录边界等明确失败。

`SysStateStore::save` 是同步追加但尚未同步到稳定介质；其 `flush_index()`
或 `shutdown()` 是元数据的同步屏障。两个 store 没有跨文件事务：调用者若要
持久化一组系统状态及其子快照，必须成功完成两边的屏障。尚无断电测试证明；
当前证据是系统调用故障注入、同步顺序、重启及新进程读取。

## 缓存与并发

L1 保存原始字节，L2 保存压缩字节，两者按 LRU 和各自字节预算淘汰。
worker 压缩一次，生成磁盘记录并复用压缩结果填充 L2。
`load`、`load_shared` 共用读取路径；缓存无法容纳对象也不能导致读取失败。
磁盘/L2 解压后的内容检查 hash。

`load_shared` 返回真正拥有缓冲区的 `shared_ptr<const vector<uint8_t>>`。
淘汰、重载和关闭只移除缓存的所有权，不会清空调用者手上的数据。
因此缓存预算不等于总 RSS：调用者持有的缓冲区、pending、索引、压缩上下文
和分配器开销另计。L1/L2 预算为零时仍可保存和读取。

未完成写入默认最多 50 个、64 MiB；队列和正在执行的任务都计入。
允许单个超过字节预算的快照，在其他 pending 排空后执行，避免永久等待。
通过 `StateStore.max_pending_bytes` JSON 配置设置字节限制。
旧 malloc-trim 选项保留读取兼容但已失效，配置时会明确提示。

并发去重、缓存、队列和任务计数在同一短期锁下更新；生命周期锁将 init/shutdown
与调用方隔离。压缩和常规磁盘读取在缓存锁外进行。预取只改变缓存，不改变状态字节。
当前没有承诺多个独立 `StateStore` 对象可共享一个 packed 单例并独立关闭。

## 磁盘格式与维护

快照存储仍使用 `memory/segments/*.dat`、`global.idx` 和 `global.inc`。
每条记录包含 hash、压缩/原始大小和 checksum。常驻 journal FD 避免每次写入
打开文件；正常 save 不逐条 fsync。段轮转先同步旧段，完成屏障同步当前段和日志。
完整索引用临时文件加原子 rename 发布；保留追加日志，避免先截断日志造成丢失。
段信息从目录发现，不再依赖可能过期的 `segments.info`。

packed 默认独占写入者；`Config.read_only=true` 明确选择只读打开。
只读进程初始化时要求写入者已完成屏障且暂不追加，不能将它当作并发 MVCC 读取。
缺失索引可从完整记录重建，重建结果在继续追加前发布。
损坏索引、未索引尾部和部分记录不会自动丢弃；应保留原目录诊断。

系统元数据仍读取原 `sstate/packed.dat` 的 checksummed record 格式，
启动时严格扫描完整日志建立内存索引。旧 `packed.idx/inc` 不再是权威来源，
保留在磁盘但忽略。使用常驻 FD，缓存最多 16,384 条且 payload 不超过 1 MiB，
不再启动时预加载全部数据。内存索引仍随状态数量增长，启动扫描耗时也随数据增长。

当前 store 无删除或覆盖 API；旧 compact 会冒险重写活数据，已移除其破坏性实现。
保留的维护接口执行验证/同步，不承诺回收文件空间。没有自动修复损坏文件的功能。
`tools/sstate_query` 和 `tools/verify_sstate` 共用只读记录扫描与语义身份验证，
无需 idx/inc，遇到坏记录失败，不修改被检查文件。

## 验证和基准

```sh
make NP=1 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4 check-storage
make NP=1 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4 check-scenarios check-futex-snapshot
make NP=1 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4 benchmark-storage
python3 tests/storage_benchmark.py build/release/np1/tests/storage_benchmark --trials 5
```

`check-storage` 覆盖缓存所有权、持有 worker 的确定性屏障测试、背压、并发去重、
多 worker、预取、重初始化、故障传播；packed 的 raw/compressed、短 I/O、
段轮转、恢复后追加、新进程只读加载和同步故障；元数据并发、碰撞、缓存淘汰、
损坏尾部和工具只读行为。原生场景另验证同步失败非零退出及 tracee 清理。

基准生成固定的 128 × 128 KiB 混合可压缩快照，比较大缓存与受限缓存；每次
读取都比较完整字节。元数据负载为 10,000 × 32 字节。每个进程使用全新目录，
结果和日志保存在 `build/storage-benchmark/`。实测与限制见
[维护记录](maintenance-plan.md)，不能把这组微基准等同于所有搜索负载。
