# CoreMQTT

`libs/pal/providers/coremqtt` 将 `@h2_vendor_coremqtt` 集成为 GizOS 的 MQTT 实现，并向上提供 `h2_pal_mqtt_api_t`。

## API Reference

[API Reference](/references/coremqtt)

`libs/pal/providers/coremqtt/include` 中实际参与项目构建的头文件是 CoreMQTT 的生产 Public API contract。Config 注入 PAL mem、network、time 和 log API，以及收发 publish record 数量。Provider 拥有输出 API 的 `user` state；调用方必须先关闭全部 client，再销毁 provider。

## 依赖和边界

CoreMQTT 负责 third-party library 与 PAL MQTT contract 之间的适配，不负责创建平台网络 backend，也不拥有 Wi-Fi、modem、TLS credential provisioning 或 broker policy。

BK7258 使用 Bazel Cortex-M33 toolchain 编译 `//libs/pal/providers/coremqtt:coremqtt`，并在该 target
内应用与 SDK MQTT 共存所需的 symbol namespace；Armino `h2_coremqtt` component
只导入 archive closure，不再维护第二份 source list 或 compile-definition loop。

## 构建与测试

```sh
bazel test //libs/pal/providers/coremqtt:all
```

`tests/` 覆盖 MQTT API、client 和 transport adapter。未标记的共同 host PAL E2E 在
Linux、macOS 和 Windows 通过 loopback broker 验证真实 `CoreMQTT -> Net PAL` 的
connect、subscribe、publish echo 与 disconnect；它不使用公网 broker、credential 或 secret。

## 发送期限

CONNECT、publish、subscribe、unsubscribe 与 disconnect 使用调用方配置的 operation deadline；短的 `process_loop` poll budget 独立。Transport 的 scatter write 在一个 PAL deadline 内提交完整 vector，处理真实 short write 与 WOULD_BLOCK，并将剩余期限传给支持 send-timeout 的 Net PAL。发送已接受的 prefix 与失败保持真实，不因 vendor 的 10 ms 默认 send-loop budget 将合法的慢阻塞发送截断，也不把 partial packet 报成成功。回归使用真实 CoreMQTT serializer、25 ms PAL write、短 prefix/WOULD_BLOCK 与 60 ms 总期限验证完整发送和超时边界。
