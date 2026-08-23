# C

本规则适用于 Public Header、PAL contract、portable library、target component、BSP、firmware entry 和 C-facing test。代码优先保持 C11 可移植性；target SDK 必须使用扩展时，把扩展限制在对应 component 或 BSP 内。

## 编译基线

Portable C code 至少应在以下 warning policy 下保持干净：

```text
-std=c11 -Wall -Wextra -Werror
```

`libs/<library>` 通过自己的 Bazel package 编译和测试。Target source 同时遵守对应 SDK compiler 的限制，但不能把 SDK type 或 macro 泄漏进 portable public contract。

## 文件边界

- Public declaration 只放在 package 的 `include/`。
- Implementation 和 private state 放在 `src/`；private header 不加入 public include path。
- 一个 source file 聚焦一个明确能力，不使用泛化的 `common.c` 收集无关函数。
- Public header 只 include contract 必需的最小依赖，不能传递暴露 SDK、board、launcher 或 generated config header。
- Public header 使用 include guard；可能被 C++ 使用时提供 `extern "C"` guard。
- `.c` 文件优先 include 自己对应的 header，以尽早发现 contract 缺失。

## Public API

Public API 必须明确：

- 输入、输出和可选参数。
- Pointer 是 borrowed、retained、owned 还是 caller-provided storage。
- Instance 的 init/open、close/deinit 和重复调用语义。
- 函数是否阻塞、timeout 单位以及允许调用的 callback/task context。
- 成功、unsupported、timeout、would-block、closed 和 partial failure 的返回语义。

Public Header 中的 Doxygen 注释是 API Reference 的 source of truth。修改 declaration 时同步修改 `@brief`、参数、返回值、ownership 和 lifecycle 说明，不在 Markdown 中复制第二份函数声明。

## API Object 和 State Object

PAL capability 使用 `user + vtable`：

```c
typedef struct h2_pal_example_vtable {
    h2_pal_result_t (*operation)(void *user, h2_pal_example_value_t *out_value);
} h2_pal_example_vtable_t;

typedef struct h2_pal_example_api {
    void *user;
    const h2_pal_example_vtable_t *vtable;
} h2_pal_example_api_t;
```

- API object 描述可调用能力，调用方通常通过 `const <owner>_api_t *` 借用它。
- Vtable 是只读 dispatch table，使用 `const` pointer。
- Mutable handle、connection、track、stream 或 device instance 是 state object，不因为内部含 callback 就命名为 API。
- API object 不隐藏在 global singleton 中，通过 config、runtime field 或函数参数显式传递。
- `libs/pal` 只定义 contract，并提供 canonical unsupported API object；不提供真实 backend、额外 status backend、fake implementation 或 global forwarding proxy。
- Public static inline helper 可以完成参数检查和 vtable dispatch；缺少 API、vtable 或 operation 时返回明确的 unsupported result。

## Type 和 Const

- Size 使用 `size_t`；wire、storage 和 hardware register 使用明确宽度的整数类型。
- 不假设 `int`、pointer、enum、alignment、endianness 或 struct padding 在 desktop、ESP 和 BK 上一致。
- 只读输入使用 `const`。函数需要修改 instance state 时不能错误地添加 `const`。
- Public struct field 只在调用方确实需要构造或读取该数据时暴露；implementation state 使用 opaque handle 或 private pointer。
- Wire format 不直接序列化 C struct，必须逐字段编码并明确 byte order、长度和范围。
- Enum 用于有限且稳定的状态集合；接收外部数据后先校验再转换为 enum 语义。

## 参数和返回值

- 函数入口先验证 required pointer、length、enum range 和 state。
- Output 参数在失败时不能保留可能被误用的旧值；适用时先写入 `NULL`、`0` 或 empty value。
- 每个 allocation、PAL callback、SDK call、encode/decode 和 transport operation 都检查返回值。
- 错误使用已有 domain result；不要把不同错误全部折叠为 boolean。
- Unsupported capability 明确返回 unsupported，不静默成功。
- 使用 early return 处理 validation 和 error path，保持主流程直接。

## 生命周期和清理

- Init/open 失败时清理已经获得的全部资源。
- Cleanup 必须容忍 partial initialization，并在 contract 允许时支持 `NULL` 或重复调用。
- Borrowed dependency 的生命周期必须覆盖使用它的 instance；deinit 不能销毁不属于自己的 backend。
- Callback registration、task、queue、timer 和 transport 必须有对称的 unregister、stop、close 或 release。
- Close/deinit 后不能留下仍会访问已释放 state 的 callback 或 worker。

## 内存和边界

- Pointer arithmetic、allocation、copy、append 和 NUL termination 前先验证长度及 overflow。
- Callback 收到的 buffer 默认只在 callback 期间有效；需要保留时复制到 owned storage。
- Stack buffer、decoder scratch 和临时 frame 不能逃逸。
- Byte buffer 转换为 typed pointer 前确认 alignment、size、lifetime 和 aliasing。
- 固定容量 queue 或 ring buffer 必须定义 full、empty、drop 和 backpressure 行为。

## Callback、Task 和并发

- Callback 只完成 contract 允许的最小工作，不在 ISR-like callback 中 sleep、等待或动态分配，除非 target contract 明确允许。
- 跨 task 访问的 state 必须有清楚的 owner、同步方式和 shutdown 顺序。
- Producer/consumer queue 必须明确谁创建、谁关闭、谁释放以及关闭后调用的结果。
- Public API 不能暗示 thread-safe；只有实现和文档共同保证时才声明并发安全。

## 注释

- 注释解释 ownership、协议约束、硬件假设、状态转换原因和不明显的边界条件。
- 不复述下一行代码，不保留已经失效的实现历史。
- TODO 必须说明缺失行为或关联 issue，不能代替错误处理。
- Target-specific workaround 说明受影响的 SDK、chip 或 errata 条件。

## 验证

Portable library 从 repository root 运行：

```sh
bazel test //libs/<library>:all
```

Target 或 board code 还需要执行对应 firmware build。所有 C 改动最后运行 `git diff --check`，并确认没有无关格式变化。
