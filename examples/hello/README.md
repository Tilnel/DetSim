# Hello

目标代码和驱动合一，用于最小原生程序的手动编译与执行。这里没有性质插件，
也不把一次原生执行当作完成模型检验。

```sh
make -C examples/hello CC=/usr/bin/gcc
./build/examples/hello/hello
```

可执行文件输出到 `build/examples/hello/`，不再提交预编译ELF。
需要受控执行、保存恢复及带性质的完整配置时，使用
[`runtime_admission`](../runtime_admission/README.md) 的生成 `config.json` 流程。
