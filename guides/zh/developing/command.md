# Command

`libs/command` 提供同步、可注册的命令执行器。调用方传入 byte stream I/O vtable 和全部工作区，注册命令路径与 handler，然后反复调用 `h2_command_poll()`；每次 poll 最多读取并执行一条完整命令，handler 返回后统一 flush 输出。

## API Reference

[API Reference](/references/command)

命令实例、注册表、输入缓存和 argv scratch 都由调用方持有。Library 不创建 task、不保存全局实例，也不拥有 I/O backend。首次调用 `h2_command_execute()` 或 `h2_command_poll()` 后注册表冻结；同一 stream 按“读取一条、同步处理、返回一条”的顺序执行。

Handler 可以通过 `h2_command_read()` 或 `h2_command_read_exact()` 同步读取紧随命令行之后的 payload，并通过 `h2_command_write()` 输出结果。执行器会保留读取命令行时预取的 payload 和下一条命令字节。

已经由 target SDK 或其他 parser 拆分成 argv 的命令可以调用 `h2_command_execute()`，直接保留参数边界；adapter 不需要把 argv 重新拼接成命令行再解析。

## 边界

- I/O contract 属于 `libs/command`，不新增 PAL command contract。
- Library 不依赖 BLE、iKCP、io_stream、UART、USB 或网络实现。
- ESP-IDF、Armino CLI 等 target adapter 只负责把实际 transport 映射到 command I/O vtable。
- Transport 是否支持并发、重连或复用由调用方决定；单个 command 实例本身按同步 stream 使用。

## 构建与测试

```sh
bazel test //libs/command:all
```
