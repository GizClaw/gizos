# OneNET 设备 E2E App

Portable App 通过调用方注入的 MQTT、JSON、Memory 和 Time PAL 运行公共 OneNET 库。它拥有一次连接与订阅确认、一次属性上报与平台应答、客户端关闭和清理；完整通过要求这些阶段都成功。身份、Token、属性模型与 provider 生命周期由 launcher 提供，App 不读取环境变量、文件或具体 SDK。接口与终态结果见 [Public Header](app/include/h2_onenet_e2e.h)。

接入失败时不会继续上报，也不执行自动重连。收到控制指令时回复未实现，不驱动物理执行器；平台拒绝、超时与资源清理失败保留为失败。Host example 与 DevKit carrier entry 消费同一个 App，host loopback 测试运行真实 CoreMQTT、Net 与 JSON 路径：

```sh
bazel test --config=macos_arm64 //projects/example/targets/cc_binary/onenet:wire_test
```

Host 示例的参数和环境变量见该入口的 usage。实网调用使用平台登记的产品、设备与对应 Key 或 Token，开启 TLS 并验证服务器证书。灵犀的产品身份与 Key 不会自动成为 OneNET 凭据；兼容性尝试必须由调用方明确授权并分别配置。OneNET 实网属性上报还需要该产品已定义的属性模型，loopback 成功不代表真实平台登记或设备入库通过。

DevKit 的运行时配置、托管安装与恢复流程见 [carrier entry](../../targets/h2loader_tar_zlib/carrier/devkit/README.md)。设备与 host 的结果分别记录，不能从 host 结果推断实板通过。
