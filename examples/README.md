# 模型检验案例

每个案例归一个目录。该目录拥有目标源码或外部依赖、运行库的驱动、DetSim
选择点/性质插件、运行配置及说明；目标和驱动可以合为同一个程序。宿主侧的
编译、控制执行、搜索和验收脚本也属于案例，放在这里。

| 案例 | 目标与驱动 | 插件、配置与入口 |
| --- | --- | --- |
| [two_debits](two_debits/README.md) | C/C++两次扣款，正常/注错版本 | `input_checker.cpp`；`run.py`枚举输入并执行原生CLI搜索 |
| [lost_notification](lost_notification/README.md) | pthread条件变量通知丢失 | `deadlock_checker.cpp`及复用的完成标记插件；`run.py` |
| [runtime_admission](runtime_admission/README.md) | C/C++/Rust/Go/Python/Java准入探针 | `admission_checker.cpp`；`run.py`，不支持的运行时保持非绿色 |
| [exact_once](exact_once/README.md) | Rust/Python/Go生产者消费者 | `exact_once_checker.cpp`；`run.py`；输出协议检查，不解码Python对象 |
| [raft](raft/README.md) | Willemt Raft库与三节点驱动 | `raft_fixture.c`、`plugins/`、`raft_time_choice.cpp`及 `run.py`；保留原手动JSON配置 |
| [redisraft](redisraft/README.md) | Redis维护的Raft C库与三节点驱动 | `plugins/`、fixture及 `run.py`、`quorum.py`、`repair.py`；不是完整Redis module |
| [redisraft_module](redisraft_module/README.md) | Redis服务加载RedisRaft module，加RESP客户端 | `client.c`、`checker.cpp`、`time_choice.cpp`及 `run.py`；模型验收仍有未完成项 |
| [hello](hello/README.md) | 最小程序，目标和驱动合一 | 手动示例，无性质插件，不宣称已模型检验 |

`bakery`、`brk`、`bt`、`cast_test`、`complex`、`exception`、`expr_test`、`fs`、
`loop`、`loop_inf`、`malloc`、`member`、`parentheses_test`、`simple_complex`、`write`
是历史手动示例：各自的源码、Makefile和JSON在本目录内。[redis](redis/README.md)保留Redis手动
配置和客户端，`paxos`保留Paxos依赖与适配实验。它们的存在不代表当前验证通过；
Paxos子模块还有未提交适配源码，见[子模块审计](../docs/repository-layout.md)。

## 配置和产物

手动案例使用目录内已有JSON。需要编译不同语言、正常/注错变体、full/pages或
不同节点数的案例，由本案例 `run.py` 生成实际配置；每个会话的 `config.json`
与日志、命令、结果、反例放在本轮输出目录。这样配置记录真实二进制、插件和
文件映射路径，不提交一份带失效绝对路径或无法直接运行的占位模板。

生成产物写入 `build/` 或显式 `--output`，不写回目标依赖。每个README列出配置
策略及复现命令；[catalog.json](catalog.json)提供机器可读的案例索引和能力边界。
测试框架自身以及检查案例验收逻辑的宿主回归仍在 `tests/`。

唯一非案例目录是 [`_support/`](_support/README.md)：复用会话控制、配置写出、
保存状态上的转换重放及编译/文件捕获工具。它不拥有目标程序、专用状态解码
或搜索策略；Raft的专用搜索属于 `redisraft/search.py`，不混进共享层。

原 `examples/cases/` 源码已拆入上述案例。当前工作区可暂时存在一个忽略的旧路径
符号链接，仅供迁移前已启动的三个长实验读取冻结源码，待这些任务结束后回收。
新运行、构建和测试全部使用新入口；新clone没有该兼容目录。
