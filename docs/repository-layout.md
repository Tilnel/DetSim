# 目录边界与子模块审计（2026-10-17）

## 本轮计划

本轮只修复构建产物与实验材料的目录边界，不改变执行、调度或状态存储语义。

1. 先运行现有构建契约测试，再为 `clean` 保留实验目录、工具按配置输出和
   benchmark 不污染源码目录添加回归。修改前确认新测试失败。
2. 将只读状态检查工具的三个源码文件归入 `tools/storage/`，更新唯一构建入口
   和启动布局诊断的共享头引用。工具与 benchmark 可执行文件统一进入
   `build/<mode>/np<N>/bin/`，继续保留原 make 目标作为构建别名。
3. `make clean` 只删除已定义的编译目录及旧版固定位置可执行文件，不再删除
   整个 `build/`。实验回收按独立清单执行，不提供笼统的目录递归删除目标。
4. 运行构建契约、组件检查、生产构建和真实只读工具回归，独立审查后提交。

## 当前目录职责

| 目录 | 职责 | 管理约束 |
| --- | --- | --- |
| `src/` | 通用框架实现及公开插件 ABI | 不加入目标系统特判 |
| `tests/` | DetSim 自动验证、fixture 与对应 make 片段 | 不能把 workload 自测当框架验证 |
| `examples/<case>/` | 每个案例的目标/外部依赖、驱动、插件、配置和运行入口 | 实际model check相关源文件归案例；生成物写入build/output |
| `examples/_support/` | 共享宿主会话、编译、配置写出与转换重放工具 | 唯一明确标识的非案例基础设施；不含目标状态提取或专用搜索 |
| `third_party/` | 编译依赖 | 固定版本，按需初始化 |
| `tools/storage/` | 保存状态的只读诊断工具源码 | 可执行文件位于配置构建目录 |
| `tools/`、`scripts/`、`perf/` | 其他离线工具、诊断脚本及 microbenchmark | 后续逐项核验调用关系，避免只搬目录 |
| `build/release/`、`build/debug/`、`build/tests/` | 可重建编译产物 | 普通 clean 可以回收 |
| `build/case-jobs/`、其他实验目录 | 本地日志、验收记录与实验生成数据 | 与编译 clean 隔离；按保留策略回收 |
| `.omx/` | 当前协作状态和恢复记录 | 不作为产品状态，不混入镜像身份 |

## 子模块现状与建议

五个已初始化子模块的HEAD均与主仓库gitlink一致，没有跟踪文件改动；本地状态
检查没有访问远端，不能据此保证远端永久保留这些commit。主仓库的两个 `?`
来自未跟踪文件，不是子模块提交漂移。

| 子模块 | 锁定提交（缩写） | 本地状态与用途 | 建议 |
| --- | --- | --- | --- |
| `third_party/cereal` | `22a1b369` | 干净；编译依赖 | 保留gitlink，按需初始化；系统头回退会改变实际依赖来源，复现实验记录版本 |
| `tools/FlameGraph` | `41fee1f9` | 干净；可选profiling工具 | 不作为主构建必需依赖；未来可移到 `third_party/FlameGraph`，同时更新脚本，不必现在改gitlink路径 |
| `examples/redis/redis` | `53d25974` | 干净；本地处于unstable分支，源码版本宏是255.255.255 | 以commit标识，不能称稳定发布版；真实module案例另用 `build/workloads/redis-7.4.2`，不要混淆 |
| `examples/redisraft/raft` | `7540ef5a` | 11个未跟踪文件：10个clangd索引及 `raft` 二进制 | 属生成物，可显式清理或本地排除；不应提交缓存/二进制，也不要用ignore=all隐藏真实改动 |
| `examples/paxos/paxos` | `bd9581b8` | 12个未跟踪文件，包含Go源码、go.mod/go.sum及二进制 | 先保存并评审源码；不能整体 `git clean`。主仓库commit不包含这些文件，新clone无法复现适配 |

需要特别注意名称：`examples/redisraft/raft` 的README说明它是 **Redis维护的Raft
C库**，不是完整RedisRaft module。`.gitmodules` 的fork仓库名虽然叫redisraft，
也不能改变这一事实。`examples/raft/raft` 则是主仓库跟踪的40个vendored文件，
不是第六个submodule。两份库、完整module和Redis服务必须在案例说明中分别标识。

Paxos未跟踪源码包括 `cmd/detsim_node/main.go`、`cmd/test_sync/main.go`、
`paxos/node_sync.go`、示例Makefile/C文件及Go模块清单。`node_sync.go` 直接使用
Paxos包的私有内部字段，不能简单移动到包外就当作等价适配。推荐将必要变化
整理为 `examples/paxos/` 下可审查的patch与构建配方，应用到 `build/` 中的
disposable副本，记录上游commit和patch hash；这样无需把未知改动直接推入fork。
另一条可行路线是在fork提交经审查的变化、确保可获取，再更新主仓库gitlink。
本轮只建议，不执行发布或更改gitlink。同步单线程适配还会改变原程序交织，
必须与原生多线程Paxos测试范围分开描述。

依赖初始化应按用途选择，例如 `git submodule update --init third_party/cereal`；
不要用 `--remote` 把锁定版本随意换成分支最新提交。处理有本地源码的子模块前
先检查 `git -C PATH status --short --untracked-files=all`。本轮没有reset、clean、
checkout、拉取或修改任何子模块，也没有更改本地排除规则来掩盖问题。

## 后续结构调整的优先级

1. **案例依赖与构建配方分离。** 逐案例将外部项目固定版本、patch和manifest
   建立明确入口，在 `build/workloads/` 构建；先解决Paxos不可复现适配，再讨论
   统一 `workloads/` 命名。不要移动当前三个在途RedisRaft任务的任何依赖。
2. **一个活跃搜索入口。** `src/core/scheduler.cpp` 与
   `src/core/scheduler/exploration.cpp` 同时编译，但CLI走前者。目录合并必须伴随
   successor-set与性质观察等价测试，不能把搬文件当作解决重复实现。
3. **状态层按职责拆分。** `core/state/` 同时拥有语义状态、捕获恢复、reader和
   物理存储；先梳理依赖和恢复次序，再分 `snapshot` 与 `storage`。保持一种行为
   的唯一实现，避免再造仅转发的抽象层。
4. **测试与文档入口。** `tests/` 的平铺文件和逐文件忽略例外不便维护，历史文档
   与当前契约混排。按单个模块迁移测试和make片段，验证全部路径；文档先建立
   导航与历史标注。不要一次搬完，也不广泛放开忽略规则暴露本地文件。

本轮只实施计划中的构建产物边界及storage工具源码归类。`src/`、案例驱动、
子模块路径、快照格式、运行时与探索算法均不改变。

### 额外发现：旧perf基准尚不能作为验证入口

真实构建 `make NP=1 benchmark` 暴露了既有问题，不能用fake compiler契约测试
代替实际编译。`xxh_bench` 缺少 `-lxxhash` 已以独立修复补上，但
`perf/xxhash_benchmark.cpp` 给512KiB输入缓冲区读取1MiB，仍有越界风险，未运行
它生成任何性能结论。旧store benchmark也有链接时缺少 `get_ncurses_ui`、
`sysstate_store_benchmark.cpp` 缺少cstring头、调用已变更的hash签名等问题；
后者还有detached监控线程与主线程共享vector、同key保存不同字节的旧假设。
这些需要单独的行为回归及修复，不能通过目录搬迁或仅修到能编译就宣称可用。

目录调整保留了旧benchmark构建目标及配置输出规则，其真实aggregate构建目前
仍失败；本轮验证以生产tracer、只读storage工具、启动诊断编译和构建契约为准。
实际存储基准应使用已有 `benchmark-storage` 和 `tests/storage_benchmark.py`
的字节校验路径，不能把旧 `perf/` 的模拟格式当作当前状态镜像格式。

## 本轮验收

- 原有10项构建契约先通过；新增3项目录/清理契约在旧规则下失败，再在修改后
  通过。另为独立的xxhash链接修复增加1项回归，总计14项，独立审查复验通过。
- `make check CC=/usr/bin/gcc CXX=/usr/bin/g++`：9项组件案例及14项构建契约通过。
- `make -f tests/startup_layout.mk NP=1 -j4 CC=/usr/bin/gcc CXX=/usr/bin/g++ all tools startup-layout-dump`：
  真实生产tracer、两种storage工具、迁移共享头的启动诊断编译/链接通过；这只是
  编译证据，没有重新运行启动布局或探索实验。
- `make NP=1 check-store-tools CC=/usr/bin/gcc CXX=/usr/bin/g++`：实际工具的无索引/
  旧索引读取、查询、坏尾部/校验和/key/旧布局拒绝及不写入源状态库回归通过。
- Python语法编译及 `git diff --check` 通过。三份工具源码迁移前后逐字节相同；
  NP2和debug的输出隔离由fake compiler构建契约验证，未声称本轮做了NP2生产构建。
- 两个旧 `tools/` 目录内可执行文件在确认没有进程执行后删除，避免残留旧版本。
  子模块gitlink、已有未跟踪源码和在途实验未修改。

机器清单与原始构建日志位于 `build/repository-layout-audit/`（不入库），包括
子模块状态/未跟踪文件hash、旧工具回收、RED/GREEN及真实构建失败记录。
本轮没有执行实际仓库的 `make clean`；其数据保留行为在隔离fixture中验证。

## 后续修订：案例按目录自包含

用户明确原始设计：所有实际模型检验相关文件均属于 `examples/`，每个案例
拥有目标源码/库、运行驱动、选择点/性质插件及配置；目标与驱动可以合并。
因此没有采用将这些文件拆入 `tests/fixtures` 或 `tests/integration` 的方案。

原 `examples/cases/` 已拆为 `two_debits`、`lost_notification`、`runtime_admission`、
`exact_once`，Raft相关驱动归入已有 `raft`/`redisraft`，完整module独立为
`redisraft_module`。案例专属的三节点搜索与状态投影归 `redisraft/search.py`，
共享 `_support/session.py` 只保留宿主工具。三个单进程案例不再经由一个
`--mode` 总入口调度；各自 `run.py` 拥有配置策略及验收流程。不同语言/变体
配置由驱动生成，精确 `config.json` 与本轮结果一同保存，避免失效路径模板。

根目录散落的hello源码收拢到 `examples/hello/`，预编译ELF退出版本控制，
编译输出到 `build/examples/hello/`。宿主回归留在 `tests/`，案例README、
实际目标、插件、配置和入口不因此移出案例目录。索引为 `examples/README.md`
和 `examples/catalog.json`。

三个在途module driver仍持有原路径。迁移前对41个旧源文件做逐字节冻结并
记录inode/进程实例，用已在隔离目录验证的Linux `RENAME_EXCHANGE` 将旧目录
原子交换为指向 `build/legacy-cases` 的本地忽略链接；旧原目录也保存在
`build/original-case-tree`。交换前后41个源文件hash不变，三组driver仍为原实例。
新入口完全不依赖该链接，新clone没有 `cases` 源码或兼容目录。待这三组旧任务
结束后可回收链接和两个旧副本；不要在它们仍运行时递归清理这两个兼容目录。

本轮的RED/GREEN、宿主回归、真实案例结果及兼容桥清单位于
`build/example-layout-audit/`。历史报告中的旧源路径保持原样，以免改写执行归属；
复现时应选择匹配的历史commit，当前运行入口以案例索引为准。

同一规则还应用于原来散落在 `tests/` 的实际案例运行工具：Redis手控驱动归
`examples/redis/run.py`，Raft的benchmark、profiling汇总、page/soft-dirty实验和
seed插件归 `examples/redisraft/`，zero-guard实验归 `examples/two_debits/`。
对应 `benchmark.mk` 也归案例，插件产物在 `build/<mode>/npN/examples/redisraft/`。
框架内部的actor/恢复/存储回归仍留在 `tests/`，不把它们伪装为实际系统案例。

本轮验证：

- 迁移前后9组宿主回归，合计103项通过；独立审查复验通过。
- `make check CC=/usr/bin/gcc CXX=/usr/bin/g++`：9项组件案例、14项构建契约及
  5项目录/入口检查通过。后者核验14个实际运行入口与profile入口的help、源码
  归属、目录外启动、catalog和make路由，不启动被测进程。
- `make NP=1 check-cases CC=/usr/bin/gcc CXX=/usr/bin/g++`：本轮生产NP1的
  C/C++扣款8行及pthread通知4行full/pages真实搜索、终点/反例三次重放通过；
  runtime-admission实际C探针的bounded/成功/缺少工具回归通过。
- Willemt及Redis维护Raft库各4行正常/注错full/pages实际搜索和三次重放通过。
  为避免替换在途工具及无关重编译，这8行明确使用已有不可变
  `tracer-iteration23-np3`，不声称是新C03实现的NP3验证。
- 案例新路径上的NP3选择/性质/seed插件真实编译通过；hello从源编译并打印
  `Hello`。新增搬迁的历史benchmark只做入口、路径和编译检查，没有重跑完整
  性能矩阵，也没有重启或重新验收完整RedisRaft module。

本轮小案例结束后，其生成的原始状态库按独立清单回收，仅保留配置、结果、
源码版本、日志及重放记录；不因目录调整再次长期堆积探索镜像。三个在途任务
及旧源兼容桥不在该回收范围。

另将待提交文件导出到无兼容链接、无已初始化子模块的独立目录，5项目录/入口/
构建路由检查全部通过，避免本机残留文件掩盖漏提交或旧路径依赖。清理实际
回收104个验证状态库约998.3MiB，220个结果/日志文件hash不变。迁移机器记录与
执行版本边界已在独立审查中核验；这些验证不扩大原有框架正确性claim。
