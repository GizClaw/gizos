# Go

本规则适用于 `tools/` 中的 host command、generator、protocol probe、local service 和其他 Go package。Go code 遵循标准工具和惯用写法，不建立只在 GizOS 内部才成立的语言习惯。

## 格式和包

- 修改过的 Go 文件使用 `gofmt` 或 `go fmt`。
- Package name 简短、小写，并与目录 owner 一致。
- Import 使用标准分组，交给 `gofmt` 排版。
- Side-effect import 必须是必要的 registration，并用注释解释原因。
- 避免 package global 和 `init()`；required dependency 通过 constructor 或 config 显式传入。
- 只导出其他 package 需要使用的 contract，不为测试导出 private helper。

## API Shape

- Function signature 表达 ownership、blocking、cancellation 和 error behavior。
- Constructor 优先返回 concrete type；只有 consumer 需要替换实现时才引入 interface。
- Interface 定义在 consumer 一侧，并保持最小方法集合。
- Type 的 zero value 在合理时应可用；required dependency 不能依赖隐藏初始化。
- Pointer receiver 用于修改 state、避免复制大对象或保持 method set 一致；小型 immutable value 可以使用 value receiver。
- Embedding 只表示真实组合，不能借此暴露不属于 public API 的 field 或 method。

## 控制流

- Validation 和 error path 使用 early return。
- `return`、`break`、`continue` 或 `panic` 后不增加多余 `else`。
- Function 聚焦单一职责；复杂 protocol state 或 cleanup 拆成有明确 contract 的 helper。
- Named result 只在能改善文档或 defer cleanup 时使用；较长函数不使用 naked return。
- `defer` 用于保证资源释放，但不能隐藏 caller-visible error。

## Context 和错误

- 可取消操作的第一个参数是 `context.Context`。
- Subprocess、network request、serial operation 和长时间 filesystem walk 传递 context。
- 普通 parse、validation、I/O 和 protocol failure 返回 error，不使用 panic。
- 使用 `%w` 保留可检查的底层错误，并补充失败的 command、path、peer 或 protocol step。
- 只有调用方确实需要分支处理时才定义 sentinel 或 typed error。
- Error message 保持稳定、简洁，并避免泄漏 secret、token 或不必要的本地绝对路径。
- `recover` 只用于 process、goroutine 或 plugin boundary，把 panic 转换为受控失败；不能吞掉普通 bug。

## 并发和关闭

- 每个 goroutine 都有明确退出路径。
- Channel 的发送方、接收方和 close owner 必须清楚；receiver 不随意关闭 channel。
- 使用 context cancellation、wait group 和显式 close 等待 worker 退出。
- Loop 中不使用会持续创建 timer 的 `time.After`；复用并正确 stop timer 或 ticker。
- File、socket、serial handle、subprocess、timer 和 server 都有对称 cleanup。
- 只有确实改善 latency、throughput 或 isolation 时才增加并行；简单同步调用不强行包装为 goroutine。
- Shared state 选择 channel 或 mutex 时以 ownership 清晰为准，不为了形式统一强制使用其中一种。

## 数据和文件

- 校验 file path、archive path、network input、serial input、JSON、XML 和 protocol payload。
- Slice 被保留、返回或异步使用时明确 ownership 和 aliasing。
- Generated output 必须 deterministic：顺序和格式稳定，不写入本地绝对路径或无 contract 的 timestamp。
- 写入重要仓库文件时使用临时文件加 atomic replace，避免 partial write 损坏原文件。
- Command output 区分 machine-readable data、user-facing message 和 diagnostic log，不能混在同一协议 stream 中。

## 测试

- Parser、serializer、argument、path、timeout、retry、cancellation 和 malformed input 优先使用 table-driven test。
- Test name 表达行为和条件，不复述 implementation function name。
- Unit test 不访问真实外部网络或私有服务；需要 integration test 时显式配置并隔离。
- Concurrency test 覆盖 cancellation、blocked shutdown、resource cleanup 和 race-sensitive path。
- Golden file 需要可审查的 diff 和明确 update command。

## 验证

在对应 module 运行：

```sh
gofmt -w <touched-files>
bazel test //tools/webrtc-test-server:webrtc_test_server_test
go vet ./...
```

如果 package 没有独立 `go.mod`，从拥有它的 Go module root 执行命令。不要因为验证一个 package 而格式化无关 module。
