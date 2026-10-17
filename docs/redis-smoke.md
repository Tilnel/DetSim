# Redis 实测：2026-10-14

当前结论：修复 DWARF 类型查找和 accept 内存边界后，**原始带调试信息的
Redis 二进制已经在 DetSim 内完成 SET x 1 / GET x**，客户端精确 RESP 断言
通过，并以状态 0 退出；tracer 正常结束。无需剥离调试信息。

这是仓库内定制 Redis、两个单线程进程、固定手动调度、C locale 的验证，
尚不代表多线程、任意客户端、状态回溯、持久化或自动探索已正确。
Redis 子模块未修改。下面保留最初失败证据，最后一节记录修复与复现命令。

## 对象与隔离

- Redis：`examples/redis/redis/src/redis-server` 现成二进制，static PIE，带调试信息。
  `--version` 输出 `v=255.255.255 sha=1abd489d:1 malloc=libc`，不能当作正式发行版。
  子模块 HEAD 为 `53d25974f`（single thread redis）；未重新编译 Redis，不能
  从这个 HEAD 推定现成二进制准确对应的源码。
- DetSim：本轮源码，GCC 14.2，按 `simple_start.json` 的节点数构建 `NP=2`。
- 客户端：从 `examples/redis/client.c` 编译到隔离目录，测试 SET x 1 / GET x。
- 原生测试只开临时 Unix socket，`port 0`，关闭持久化，不访问已有 Redis 服务。
- DetSim 在独立工作目录和进程组中运行，设置超时，结束后清理所属进程组。
  诊断副本运行还限制地址空间为 2 GiB、关闭 core dump。未修改主机 ASLR。

## 修复前实测结果

| 测试 | 结果 | 边界 |
| --- | --- | --- |
| 原生 Redis | PING → `+PONG`，SET → `+OK`，GET → `$1\r\n1\r\n`，正常退出 | 证明现成二进制及基本命令可用 |
| 原始带调试信息的 Redis + DetSim | 45 秒内未出现交互提示符；停在 DWARF 类型构建；33 秒采样 RSS 6,539,052 KiB、CPU 约 100% | 被测进程尚未启动，不能算 Redis 启动成功 |
| 去除调试信息的副本 | 能初始化，执行 95 次服务端转换后 `exit_group(1)` | 虚拟文件系统没有环境所需的 en_US locale 文件，Redis 报 locale 初始化失败 |
| 同一诊断副本 + `LC_ALL=C LANG=C` | 服务端单步 150 次进入 epoll_wait；客户端 connect 成功并发送 SET；客户端第 23 步附近仍在等待响应，随后 tracer SIGSEGV | 未收到 SET 成功响应，GET 未完成 |

去除调试信息仅用于隔离故障，不是可交付修复：这条路径缺少正常的变量/类型
检查能力，更不能用它证明状态恢复或模型检查正确。

## 修复前定位的阻塞点

1. **DWARF 大型真实输入。** 日志显示 `Parsed 0 global variables` 后没有到达
   `Cached ... types`。GDB 在 `cache_type_by_name` 捕获的调用链是
   `cache_type_by_name → parse_type_info → dwarf_init → main`。
   源码中的嵌套类型查找会重新扫描同一个 DWARF CU 迭代器，且遍历没有释放
   多个 DIE/attribute；这是下一步需要最小复现验证的原因候选，尚未断言
   已确定唯一根因。
2. **accept 的 guest/host 指针边界。** GDB 确认 tracer 在
   `SockState::do_accept` 内收到 SIGSEGV，调用者为网络 syscall handler。
   `src/subsys/net/sockstate.cpp` 的 `socklen_t len = *addrlen` 直接读取了
   tracee 地址；应该通过 guest-memory 接口读取，还需要覆盖长度回写、截断
   和错误地址语义。tracer 崩溃后出现的 Redis epoll/accept 错误不能当作
   已发现 Redis 自身缺陷的证据。
3. **环境与模拟缺口。** C locale 能绕过本次缺失的 locale 文件；日志仍显示
   标准流 `write(1, ...) = -EBADF` 和不支持的 IPv6 socket，需分别验证模型。

## 本次产物

当时本机运行产物位于 `build/redis-probe/`，未纳入版本控制。当前 `make clean`
保留实验目录；历史镜像是否仍在，以后续[清理清单](cleanup-2026-10-17.md)为准：

- `native-result.json`、`native/redis.log`：原生控制组。
- `detsim/result.json`、`detsim/transcript.log`：原始启动失败。
- `dwarf-stack.log`：DWARF 调用栈。
- `detsim-no-debug/`：locale 失败的配置、结果与完整轨迹。
- `detsim-c-locale/`：监听、连接、SET 和崩溃轨迹。
- `accept-stack/gdb.log`：accept 崩溃栈及对应 batch 命令文件。
- `tracer-np2`、`redis-server-no-debug`、三个 `detsim_redis_probe*.py`：本次诊断副本。

所有运行使用生成目录中的独立配置，FSMap 指向原有 `fs_root`，没有覆盖它。
仓库根目录的 `tracer` 已恢复为先前的 NP=1 构建；NP=2 副本保留在产物目录。

当时的验收顺序（前四项现已完成）：大型 DWARF 有界初始化 → 原始调试版 Redis 进入事件循环 →
accept 跨进程内存回归 → 完整 SET/GET → 保存/恢复后重放相同请求。

## 修复前固定手控调度复查

针对“是否因为多线程交错过多”的疑问，又使用同一无调试信息诊断副本和
C locale，显式执行以下固定路径，完全没有运行 BFS/DFS：

1. `sw 0`、`thread 0`，只推进 Redis 150 次 `si`，到达监听后的 `epoll_wait`。
2. `sw 1`、`thread 0`，只推进客户端两次 `si`：socket、connect，连接成功。
3. 冻结客户端（尚未发送 SET），切回 `sw 0`、`thread 0`。
4. Redis 再推进到第 23 次单步尝试时，tracer 以 SIGSEGV（-11）退出。

各阶段的 `thread` 命令均只列出 `[0] TID 1 (main)`；在命令停止点读取
`/proc/PID/task`，Redis 与客户端各自观察到的最大线程数均为 1。
该固定路径约 0.83 秒复现故障，不是探索未收敛或等待超时。
这排除了交错爆炸作为**本次定制二进制、当前路径**的阻塞原因；不能推广为
普通多线程 Redis 的调度覆盖结论。原始调试版的 DWARF 阻塞仍然存在。

新增运行产物：`build/redis-probe/manual-fixed-schedule/` 内有完整
`commands.txt`、`result.json`、`transcript.log`；驱动脚本为同级
`manual_fixed_schedule.py`。未改动生产源码。

## 修复后的结果与复现

```sh
make check-accept CC=/usr/bin/gcc CXX=/usr/bin/g++
make check-redis CC=/usr/bin/gcc CXX=/usr/bin/g++
```

两者默认 NP=2。`check-redis` 会重建 tracer，并使用现成的
`examples/redis/redis/src/redis-server`，不会下载、重编译或修改 Redis。
也可以针对已构建的 NP=2 tracer 运行：

```sh
python3 examples/redis/run.py --tracer ./tracer
```

脚本每次创建独立 `build/redis-check/run-*` 目录，保存配置、完整轨迹、命令
和 `result.json`。默认 60 秒、每阶段 1000 步和 2 GiB 地址空间上限，关闭
core dump；测试结束清理所属进程组。测试客户端强制启用 assert。
失败返回非零，服务器进入事件循环不算通过。

本轮原始调试二进制的成功记录：

- Redis 启动并接受连接；客户端收到 `+OK\r\n` 和 `$1\r\n1\r\n`。
- 客户端 103 次单步、服务端累计 252 次单步；客户端与 tracer 都以 0 结束。
- 一次完整运行约 1.45 秒（含编译客户端、初始化、CLI 和落盘，不是吞吐基准）。
- 单独的原始 Redis DWARF 初始化最终源码复测约 0.368 秒，峰值 RSS 65,688 KiB；在
  1 GiB 地址空间上限、20 秒超时内完成，并验证 `sds` 类型元数据。
- accept 跨进程内存回归 12 项通过；DWARF 4/5 普通、多 CU 前向声明、
  同名 typedef/不完整类型及 C++ class 大小回归通过。

修复细节：

1. `dwarf.cpp` 一次建立类型定义偏移索引，优先完整定义；不再在解析类型时
   重置外层 CU 迭代器，并防止同名 typedef 自递归。索引遍历释放 DIE/attribute。
2. `sockstate.cpp` 使用 checked process_vm 访问 guest sockaddr/addrlen，处理
   截断、实际长度回写及 EFAULT/EINVAL；匹配进程局部 FD 对应的对端地址，
   初始化 sockaddr padding。确定失效的队首出队，不再堵住下一条有效连接。
3. `examples/redis/run.py` 保留固定手控路径，验证实际响应和退出状态。

仍需保留的边界：

- 后续 I/O 修复将 pending queue 改为 endpoint key，并保存未 accept 的数据；
  同 FD 跨节点不再混淆，格式升级到7/8，旧5/6拒绝。
- EFAULT 保留虚拟 pending connection 是当前显式模型选择；部分 guest copyout
  不回滚，不声称与所有 Linux 错误路径的副作用完全一致。
- stdout/stderr 后续改为捕获的虚拟文件；这里仍通过 socket 的实际 RESP 内容
  和客户端断言验证命令结果。
- 原始 static PIE 的运行时变量重定位、BFS、快照重放、fork/多线程和持久化
  均不在本次 Redis 成功用例的覆盖范围内。

2026-10-15 I/O 修复后复测：默认和 verify-pages 后端均通过269 server steps /119 client steps 的精确 SET/GET，客户端及 tracer 正常退出。
