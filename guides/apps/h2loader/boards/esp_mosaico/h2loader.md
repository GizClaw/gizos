# ESP-Mosaico H2Loader

普通 Loader 的 canonical 入口保持为 `projects/h2loader/targets/h2loader_tar_zlib/loader/esp_mosaico/`。 它初始化板级 Runtime、Mosaico USB console 和 Loader USB adapter，调用现有 `h2_esp_h2loader_run()`。MFG 是同一板子的[另一管理镜像](./mfg)。

## 构建与容量

使用仓库固定的 ESP-IDF 6.2 和匹配工具环境：

```sh
bazel build --config=esp32s31 \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/esp_mosaico:package
bazel build //projects/h2loader/targets/cc_binary/cli:h2loader
```

包 identity 为 `board=esp_mosaico`、`target=esp32s31`、`image=loader`、 `role=h2loader`。既有 16 MiB NOR 布局的权威输入是 `boards/esp_mosaico/esp32s31/layouts/h2loader/partition.csv` 和同目录 `sdkconfig.h2loader.defaults`；Loader 容量 3 MiB、App 容量 7 MiB。 核对实际 raw image 大小及 staging/data 可用空间后再安装，压缩包大小不能 证明镜像满足目标分区容量。

## USB 管理与更新

连接板载 Type-C；console 与协议复用同一 CDC，Loader 是唯一 RX 消费者。 以下 `h2loader` 使用同一 checkout 构建的 CLI，端口和 package 替换为实际值：

```sh
h2loader --no-ble --port <serial-port> status
h2loader --no-ble --port <serial-port> send --file <package.update.tar.zlib>
h2loader --no-ble --port <serial-port> reboot upgrade --monitor
h2loader --no-ble --port <serial-port> status
```

`send` 只接收并发布 Stage；`reboot upgrade` 才进入 AUTO 安装。 普通 Loader 与 MFG 之间均使用这条管理路径替换，保留所需的原始 package。 MFG 未完成时仍须允许管理镜像修复/替换。

Loader 自升级最终应运行在 Partition 1，active role/version/image 与新包一致， Partition 1 metadata valid 并记录新包，Stage invalid、`last_result=0`。 App 安装后应运行在 Partition 2，identity 与 metadata/package 一致并完成 Stage 收尾。命令 accepted、串口枚举和屏幕文字都不能单独证明安装成功。 完整合同见 [Loader 更新](../../update/loader)、[App 更新](../../update/app)。

没有品牌页或安装进度页。使用命令结果、日志、status 和实际镜像 identity 观察校验、安装、启动和恢复。

## 首次安装与恢复

设备尚无可用 Loader 或管理链路无法恢复时，才使用该 target 生成的 `.recovery.h2fb` 和 [CLI destructive recovery 流程](/zh/using/h2loader/cli)。 先确认实体板、USB identity 与恢复镜像；原生烧录仅用于初始配置或恢复。 可通信设备的正常更新和验收必须走管理路径。

恢复测试需记录准确 commit、package SHA-256、板 identity、USB 端口与串口证据， 覆盖候选 Loader 确认失败、回写失败及重启恢复，并重新查询最终 status。 此文档不宣称新构建或实机安装已通过。
