# DevKit 运营商设备接入 E2E

该独立 App 使用真实 ESP Runtime 与公共灵犀／OneNET E2E App，按运行时配置选择单个平台或两个平台。灵犀运行设备查询、单次心跳与临时凭据清理；心跳使用板上观察到的 Wi-Fi IP、MAC、系统与测试固件版本，未测量的网速使用协议允许的缺省值。OneNET 运行一次连接／订阅、属性上报与平台应答，然后关闭客户端与自建 CoreMQTT provider。它绑定本次取得的 Wi-Fi IPv4 地址，TLS 验证使用 Runtime Net 的受信 CA。没有固件中的真实凭据或常驻上报循环。

先准备与仓库固定 revision 一致且干净的 ESP-IDF，再构建不含接入配置的通用产物：

```sh
bazel build --config=esp32s3 \
  --//tools/bazel:firmware_version=carrier-devkit-<unique-version> \
  //projects/e2e/targets/h2loader_tar_zlib/carrier/devkit:package
```

接入信息只由本机进程环境注入。只设置选定平台的一组完整配置，缺项失败；两个平台的身份和凭据始终分别提供。设备入口从硬件组装心跳，因此不读取 host 心跳 JSON 或来源 IP 覆盖值。必需与可选环境变量从 provisioner 的 usage 读取：

```sh
bazel run --config=macos_arm64 \
  //projects/e2e/targets/h2loader_tar_zlib/carrier:provision -- --help
```

OneNET 可通过 `ONENET_CA_FILE` 提供从运营方官方来源取得的 PEM 信任证书。默认校验 endpoint 名称；旧平台证书使用服务身份时，可在显式 CA 下通过 `ONENET_TLS_SERVER_NAME` 提供预期证书身份。先核对官方证书、有效期、指纹和预期身份，再运行接入；校验失败时停止，不自动更换身份或关闭验证。该配置仍经过真实 Net PAL 的强制证书与身份校验，不修改全局 CA 或 PAL 的验证规则。官方接入地址与证书入口见 [OneNET MQTT 设备连接](https://iot.10086.cn/doc/aiot/fuse/detail/919)。

在调用方的 macOS／Linux 本机受控目录创建私有数据包；输入为上一步的 `app.bin` 与 `.firmware.json`，两者的大小和哈希必须匹配。该工具只执行打包，不调用构建、串口或平台接口。环境变量不进入 Bazel action，输出文件从创建起只允许当前用户读取，并拒绝覆盖已有文件：

```sh
bazel run --config=macos_arm64 \
  //projects/e2e/targets/h2loader_tar_zlib/carrier:provision -- \
  --app-bin /absolute/path/to/app.bin \
  --metadata /absolute/path/to/devkit-carrier-esp32s3.firmware.json \
  --output /absolute/private/directory/carrier.update.tar.zlib
```

工具借用 H2Loader 的公共 artifact writer，把配置装入一次性 Data 文件。设备读取后删除该文件、擦除临时解析缓冲区，在测试结束时擦除配置内存。Token 由 caller 提供或在板上通过 Crypto PAL 从显式 OneNET Key 生成。打包时的主机时间用于校准 TLS 和 Token 时钟，因此应在测试前打包；包与临时凭据不能提交、上传到 CI 或放进共享构建缓存。配置解析与预算的事实来源是 [runtime input parser](../../../../libs/carrier-device/src/h2_carrier_device_inputs.c)，环境映射见 [provisioner](../provision.py)。

设备操作遵守 [DevKit H2Loader 流程](/apps/h2loader/boards/devkit/h2loader)。安装前定向确认 UID、P1、P2、Stage、Data、coredump 与保存的 Wi-Fi，保留可验证的原 App／Stage 恢复包。入口仅以非保存方式使用设备已有 Wi-Fi 配置。通过 H2Loader 传入私有包、核对 Stage 身份和哈希，再托管升级并收集全新的 BOOT、selection、run ID、case 与终态结果。DNS、TCP、TLS 和 MQTT CONNACK 的观察只记录非敏感状态，不改变 provider 返回值。

每次启动只执行一次选中的平台流程，后续输出重放同一账本，不重复请求。仅在所有选中流程和清理成功后确认 App；失败镜像保持管理通道可用且不确认。未选中的平台没有通过资格。平台业务成功、网络连接成功、鉴权成功与属性模型被接受分别报告；没有收到 CONNACK 时不能把一般错误或超时解释成 Key 被拒绝。

测试后通过 H2Loader 恢复原 App、原 Stage 与 Loader 启动状态，并重新核对 P1、Data、Wi-Fi 和 coredump。最终报告绑定实际执行的 source、image/package 哈希与 run ID；构建、loopback 或其他镜像的结果不能替代该次实网观察。该入口不执行九天模型请求，不代表 H106 固件接入或入库验收。
