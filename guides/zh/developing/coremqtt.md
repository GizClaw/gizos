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

每个 public client operation 在调用 vendor 前清除上一轮发送错误并建立独立 send phase。首次非空 `send`/`writev` 从 monotonic time 建立绝对 deadline；同一次 vendor operation 的所有后续 transport callback、vector、short write 和 WOULD_BLOCK/TIMEOUT 重试共享该 deadline，不在 callback 边界续期。零长度 callback 不发送、也不启动 deadline。该 send phase 不把 TCP connect、TLS handshake 或 CONNACK/ACK 等待合并成一个新的 end-to-end timeout；它们保留既有各 stage 预算和 lifecycle。

| operation | send budget 优先级 |
| --- | --- |
| CONNECT | `config.connect_timeout_ms` > `config.operation_timeout_ms` > 1000 ms |
| publish/subscribe/unsubscribe/disconnect | 非零 per-call `timeout_ms` > `config.operation_timeout_ms` > 1000 ms |
| process 中的 keepalive/PUBACK 等自动发送 | `config.operation_timeout_ms` > 1000 ms，独立于 process 的 recv/poll 参数 |

零 public timeout 表示上述 fallback，不能将其解释为无期限阻塞或立即失败。每次非空 PAL send 前检查绝对期限；到期后不再调用 Net，保存 `H2_PAL_ERR_TIMEOUT`。有效请求只把非零的剩余 ms 传给 `tcp_send_timeout`。正返回值只计入实际接受的 prefix；短写继续使用剩余期限，WOULD_BLOCK/TIMEOUT 不代表 bytes 已接受，重试的 1 ms sleep 同样计入总预算。已有 terminal error 不被后续 vendor callback 清除；有 prefix 时 transport 返回真实 prefix，没有 prefix 时返回 -1，public operation 对 vendor MQTTSendFailed 穿透保存的 PAL error，不将 incomplete packet 报为成功。0-byte Net return 映射 CLOSED，非法 over-report 映射 IO。

`tcp_send_timeout` 对通用 Net PAL 仍是 optional；CoreMQTT 的有界发送明确要求它。缺少 callback 时在 MQTT bytes 发送前返回 UNSUPPORTED；callback 返回 UNSUPPORTED 时保存该结果。两者都禁止回落到可能无界阻塞的 `tcp_send`。Create/open 不将通用 Net provider 判为无效；CONNECT 遇到该错误关闭已建立的 TCP/TLS handles，并可随后关闭 client/destroy provider。平台必须提供遵守剩余期限的 timed send callback 才支持这项 CoreMQTT 能力；本改动不修改平台 provider 或假称未运行平台已通过。

Focused host 回归使用真实 vendor serializer/ProcessLoop 与可控 Net fixture，覆盖 25 ms send、positive short prefix、prefix 后 WOULD_BLOCK、60 ms total deadline、跨 callback 不续期、0/fallback/expired、实际 PINGREQ/PINGRESP 下 1 ms poll 与 100 ms send budget、missing/UNSUPPORTED timed capability 且 legacy call=0、failed CONNECT socket cleanup 和最终 allocation=0。这些是 provider contract 回归；硬件/真实 broker 的独立资格必须绑定其实际 source/artifact，不能由这些 tests 或历史 receipt 重绑定到新 revision。

TCP transport setup 使用 DNS 列表的实际地址族。DNS、各次 TCP attempt 和 TLS 共用原始 transport-setup deadline；pending `TIMEOUT`/`WOULD_BLOCK` 继续同一 socket，不把非末尾候选限制为 250 ms。终态连接错误关闭该 socket 后才回退到下一地址。MQTT CONNECT 发送和 CONNACK 仍遵守既有独立 stage 预算。
