# CoreMQTT

`libs/pal/providers/coremqtt` 将 `third_party/coremqtt` 集成为 GizOS 的 MQTT 实现，并向上提供 `h2_pal_mqtt_api_t`。

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
