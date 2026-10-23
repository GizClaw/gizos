# 灵犀设备接入

`libs/china_mobile/lingxi` 是中国移动终端智能体服务管理平台的设备 HTTP client，负责查询已登记设备的信息与凭据，以及单次心跳上报。它通过调用方注入的 HTTP、JSON 和 Memory PAL 工作，与 [OneNET MQTT client](./onenet.md) 独立。它不调用九天模型，也不创建平台产品、导入设备或修改产品的设备编码类型。公共接口与输入预算以 [Public Header](https://github.com/GizClaw/gizos/blob/main/libs/china_mobile/lingxi/include/h2_lingxi.h) 和生成的 [API Reference](/references/lingxi) 为准。

## 组合与身份

先准备 HTTP 与 JSON provider，再用实际产品凭据和平台已登记的设备号查询设备信息。HTTP 可以使用现有 CoreHTTP，JSON 可以使用 yyjson；最终 entry 负责 Net、TLS 和 provider lifecycle。库检查平台返回的产品和设备号与请求一致，只有 HTTP 与业务结果均成功且凭据完整时才向调用方交付。调用方持有返回的设备凭据，按产品需要存储，并在不再使用时清理。

产品编码类型、整机 IMEI、设备号和产品凭据由产品 owner 确认，库不会从 modem 推断。使用旧设备联调时必须使用该设备匹配的产品身份；旧设备接口通过不能证明另一个产品已完成入库。九天模型的访问密钥不能作为设备接入的产品 Key 或设备 Secret 使用。

生产心跳数据来自实际设备观察与产品版本，调用方负责蜂窝和非蜂窝产品的身份字段要求。库不替换元数据、不创建后台 task、不决定上报周期，也不自动重试。同步调用返回后再由 owner 更新上报状态、安排后续调用；网络失败可能存在平台已收到请求但客户端未收到响应的情况。成功响应只表示接口接受，平台设备列表与入库验收继续单独确认。

## 传输与凭据

生产配置使用运营方确认的 HTTPS origin，并由 HTTP provider 验证证书。重定向被拒绝，产品和设备凭据不能转发到其它目的地。库借用的 PAL provider 必须遵守 Public Header 中的 callback 与 TLS 要求；CoreHTTP 符合该组合方式。设备外的联调请求按平台协议提供真实来源地址字段；该字段不替代实际网络出口的授权。

凭据只经调用方的受控配置进入请求，不进入日志、Git、普通 Telemetry KV 或模型请求。接口失败分别保留 HTTP 和业务状态，调用方不能仅凭 HTTP 成功判断设备接入成功。先完成所有同步调用，再销毁 JSON、HTTP 与网络 provider。

## Host 验证

Host 示例组装同一份公共库、CoreHTTP、yyjson 和平台 Net PAL。无心跳文件时只查询设备信息；提供文件时，查询成功后发送一次调用方参数，随后退出。参数文件是平台心跳的 params 对象，字段结构从 Public Header 与 [可执行入口](https://github.com/GizClaw/gizos/blob/main/projects/example/targets/cc_binary/lingxi/main.c) 读取。入口的 usage 说明环境变量和参数；凭据从进程环境提供，不能作为 Bazel build 参数保存。

```sh
bazel test --config=macos_arm64 //libs/china_mobile/lingxi:all
bazel test --config=macos_arm64 //projects/example/targets/cc_binary/lingxi:wire_test
bazel run --config=macos_arm64 //projects/example/targets/cc_binary/lingxi:lingxi -- BASE_URL DEVICE_NO_TYPE DEVICE_NO
```

本地 wire test 只对 loopback HTTP 显式开放明文，验证真实 HTTP 编码、查询后单次心跳、业务拒绝、身份不匹配、响应预算与重定向拒绝。单元测试还验证 JSON 预算、取消和分配失败的资源回收。它们不使用公网凭据，也不代表真实平台或物理设备验收通过。

真实平台接口测试需要已登记测试设备的产品凭据、设备号及编码类型。测试者按协议自行组装心跳参数，不需要历史脚本或历史上报数据；身份字段使用获授权的测试设备身份。主机模拟接入时，使用测试机可观察的网络与系统信息，对无法取得的设备数据使用运营方协议明确允许的缺省值，并用可识别的测试版本标记说明运行来源，不能把模拟元数据描述成设备已安装的实际固件状态。生产设备验收仍须使用真实设备观察值与产品版本。

每个接口先做单次调用并检查 HTTP 与业务结果，再在平台侧核对设备和上报记录。主机接口测试无需安装固件；产品集成后的调度、身份来源、重启恢复和物理网络行为仍由产品 owner 验证。
