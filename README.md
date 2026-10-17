# DetSim

DetSim 是面向 Linux x86-64 原生程序的有状态模型检查器。它通过 `ptrace`
控制执行、模拟系统调用、保存与恢复状态，并探索多节点系统的执行路径。

项目处于恢复验证基线的阶段。已有 CLI、表达式求值、状态存储和探索实现；
多线程快照及探索正确性尚未建立端到端验证，不能将示例运行成功当作保证。
具体问题见 [正确性待办](docs/correctness-backlog.md)。

## 开发入口

先读 [AGENTS.md](AGENTS.md) 和 [开发指南](docs/development.md)。
当前调用路径、构建约定与验证范围以开发指南为准，历史优化报告仅供参考。

```sh
make help
make check CC=/usr/bin/gcc CXX=/usr/bin/g++  # 本机已验证的编译器组合
make NP=1 -j4       # 构建单节点 tracer；NP 必须与场景 Nodes 一致
make check-integration CC=/usr/bin/gcc CXX=/usr/bin/g++
make check-scenarios CC=/usr/bin/gcc CXX=/usr/bin/g++  # 需要允许 ptrace
make check-redis CC=/usr/bin/gcc CXX=/usr/bin/g++      # NP=2，现有 Redis 调试二进制
```

需要 GNU Make 4.3+、Python 3、C++20 编译器。完整 tracer 还依赖 libdw、
libdwarf、libelf、readline、zstd、xxhash、libunwind、fmt、ncurses；
nlohmann/json 已随仓库提供，Cereal 可来自已初始化的子模块或系统开发包。
Flex/Bison 用于重新生成表达式解析器。
依赖兼容性与明确的编译器选择方式见开发指南。

**已验证的基线：** GCC 14.2 与 libdwarf 0.11.1；完整 tracer 可以构建。
默认 `make check` 验证组件和构建规则，`check-integration` 验证真实 DWARF/ELF
解析，`check-scenarios` 验证九个隔离的单进程原生程序场景。多线程、标准流
模拟和一般探索正确性仍有已知缺口，详见正确性待办。

仓库内 Redis 样例已在固定手控调度下完成 SET/GET；原始调试二进制无需剥离
调试信息。验证步骤与覆盖边界见 [Redis 实测](docs/redis-smoke.md)。

## 目录

- `src/`：调度、快照、系统调用模拟、表达式及交互界面。
- `tests/`：默认自动化检查；未接入的本地历史测试不属于验证基线。
- `examples/`：被测程序、插件及外部项目，不等同于 DetSim 回归测试。
- `perf/`、`tools/`：性能测量和状态查询工具。
- `docs/`：设计、使用参考及当前维护记录。

目录职责、生成文件清理边界及子模块处理建议见[仓库结构说明](docs/repository-layout.md)。
工具和 benchmark 可执行文件输出到 `build/<mode>/np<N>/bin/`，普通 `make clean`
保留实验记录与工具链；原始探索镜像按独立清单回收。

## 设计与使用参考

- [设计原理](docs/index.md)
- [命令行接口](docs/cli.md)
- [表达式求值](docs/expr.md)
- [调用栈](docs/backtrace.md)
- [内存映射与堆](docs/brk_and_mmap.md)

运行被测程序通常需要 DWARF-4 调试信息（`-gdwarf-4 -g`）。运行场景前请先
查看开发指南中的 ptrace 隔离要求；默认检查不会运行这些场景。
