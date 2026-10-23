# OneNET

`libs/china_mobile/onenet` 是直接设备的中国移动 OneNET MQTT / OneJSON client，消费调用方注入的 MQTT、JSON、Memory 和 Time PAL。`china_mobile` 是按服务归属组织 library 的分类目录，不持有共享实现。OneNET client 处理该平台协议，不拥有产品物模型、设备凭据分发、网络选择、后台任务或固件安装。接口、容量、时间单位和 callback 生命周期以 [Public Header](https://github.com/GizClaw/gizos/blob/main/libs/china_mobile/onenet/include/h2_onenet.h) 和生成的 [API Reference](/references/onenet) 为准。

## 组合与生命周期

先准备 PAL provider，再创建 client。MQTT backend 可以选择现有 CoreMQTT；JSON backend 可以选择 yyjson。连接参数由调用方提供，产品和设备名称用于协议寻址；可选的连接身份来自实际设备，库不自行读取 modem、修改 IMEI 或猜测运营商。

调用方提供已经生成的访问 token，也可以通过公共鉴权入口借用 Crypto PAL 生成 token。该入口使用平台支持的 HMAC-SHA1，调用方负责提供校准后的绝对到期时间。使用其它受平台支持的签名方法时，直接传入自行生成的 token。密钥和 token 不进入源码、日志、物模型或上报数据。

连接后持续调用 process，收到连接完成事件后才发布业务数据。完成事件要求 MQTT 连接和订阅确认均成功；发送成功只表示 MQTT 接受报文，平台确认、错误和超时通过独立响应事件报告。调用方决定重连退避，并在需要时生成新 token；重新连接保留本实例已经消耗的上行 ID，取消旧请求并清理旧命令 handle。

所有 client 调用由一个 owner task 串行执行。库在 MQTT callback 中只复制有界输入，离开 provider callback 后才调用应用 callback。命令 JSON 只在 callback 期间借用；异步业务复制必要输入并保留 command handle，业务完成后在 owner task 回复。其它 client lifecycle 操作不得从 callback 重入。应用负责业务幂等性，不能从 MQTT 发送成功推断执行器已经生效。

client 使用创建时分配的缓冲区与请求表；输入 JSON 另外受解析预算约束。接收溢出会关闭 transport 并取消请求，避免继续报告一条已经丢失输入的连接正常。应用应按自身物模型和流量选择预算，并检查所有错误。先销毁 client，再销毁 JSON、MQTT 和其它借出的 provider。

## Host 示例与验证

Host 示例在最终 entry 中组装真实 CoreMQTT 和平台网络。它提交调用方提供的属性数据，等待平台应答；没有产品执行器的控制请求返回明确的未实现结果。接入参数、CA 和凭据由进程环境或参数提供，不写入代码。示例的输入格式见可执行入口的 usage 和 [源码](https://github.com/GizClaw/gizos/blob/main/projects/example/targets/cc_binary/onenet/main.c)。

```sh
bazel test --config=macos_arm64 //libs/china_mobile/onenet:all
bazel test --config=macos_arm64 //projects/example/targets/cc_binary/onenet:wire_test
bazel run --config=macos_arm64 //projects/example/targets/cc_binary/onenet:onenet -- HOST PORT PRODUCT_ID DEVICE_NAME
```

示例默认验证 TLS。进行本机 loopback 调试时才显式选择明文连接。wire test 使用本地 MQTT 对端，验证真实 CONNECT、SUBSCRIBE、OneJSON 上报、控制应答和断开流程；它不依赖公网账号，也不代表真实 OneNET、移动认证或物理设备通过。

真实平台验证需要调用方自己的产品、测试设备和权限。使用平台控制台检查属性与事件，并用应用模拟器下发控制；产品应用再验证从 OneNET 获取数据和经 OneNET 控制的完整路径。公共协议库的 host 测试不能替代这些验收。

## 协议来源

- [MQTT 设备连接](https://iot.10086.cn/doc/aiot/fuse/detail/919)
- [通信主题](https://iot.10086.cn/doc/aiot/fuse/detail/920)
- [属性与事件](https://iot.10086.cn/doc/aiot/fuse/detail/902)
- [设备安全认证](https://iot.10086.cn/doc/iot_platform/book/device-connect%26manager/device-auth.html)

本库实现直接设备的 OneJSON 交互；网关、旧数据流协议、平台管理 API 和 OTA 通知不在当前入口中声明支持。产品控制、凭据持久化与安装策略继续由各自 owner 实现。
