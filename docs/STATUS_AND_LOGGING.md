# Status Monitor 与日志功能

## 功能 1: 滚动区域隔离

### 实现原理
使用 ANSI 转义码设置滚动区域：
- 上方区域 (行 1 到 N-11): 允许滚动，普通输出在此显示
- 下方区域 (最后 11 行): 固定，status monitor 独占

### 代码
```cpp
/* Set scrolling region: lines 1 to rows-11 are scrollable */
printf("\033[1;%dr", rows - 11);
```

### 效果
- 普通 printf 输出在上方滚动区域正常显示
- status monitor 在底部 11 行固定显示，不会被滚动走
- 程序退出时恢复全屏滚动: `\033[r`

## 功能 2: Tee 日志 (同时输出到终端和文件)

### 实现原理
使用 pipe + 线程实现内部 tee，无需 fork() 避免影响 ptrace：
1. 创建 pipe
2. 将 stdout 重定向到 pipe 写入端
3. 启动线程从 pipe 读取，同时输出到原始 stdout 和文件

### 使用方式
```bash
./tracer -c raft.json -l trace.log
```

### 效果
- 终端实时显示所有输出
- 同时记录到 `trace.log` 文件
- 包含所有的 printf、LOG_INFO、status monitor 输出

## 完整使用示例

### 基本使用 (仅终端)
```bash
./tracer -c raft.json
```
- 启用 status monitor
- 普通输出在上方滚动区域显示

### 带日志记录
```bash
./tracer -c raft.json -l trace.log
```
- 终端实时显示
- 同时记录到 trace.log

### 后台运行 + 日志
```bash
./tracer -c raft.json -l trace.log &
tail -f trace.log
```

### 纯日志模式 (无 status monitor)
```bash
./tracer -c raft.json -l trace.log > /dev/null 2>&1
# 或
./tracer -c raft.json | tee trace.log
```

## 技术细节

### 滚动区域 ANSI 转义码
- `\033[1;Nr`: 设置滚动区域为行 1 到 N
- `\033[r`: 重置为全屏滚动

### Tee 实现关键点
- 使用 `dup()` 保存原始 stdout
- 使用 `pipe()` 创建管道
- 使用 `dup2()` 重定向 stdout 到管道
- 独立线程从管道读取，分发到终端和文件
- 不使用 fork()，避免影响 ptrace

### 与现有机制的整合
- 复用现有的 `-l` 命令行选项
- 自动检测 stdout 是否为终端 (isatty)
- 非终端模式下自动禁用 status monitor
