# BK7258 V3 202405 H2Loader

## 构建

```sh
bazel build --config=bk7258 \
  --//tools/bazel:firmware_version=<version> \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/bk7258_v3_202405:package \
  //projects/example/targets/h2loader_tar_zlib/ble-broadcaster/bk7258_v3_202405:package
```

该命令先通过内部 `:firmware` 构建 AP、CP、combined image 和 `app_ab_crc.rbl`，再通过 `:package` 生成 `update.tar.zlib`。`firmware_version` 同时注入当前固件 metadata 和 package manifest，二者不得使用不同版本。

## Partition layout

内部 8 MiB Flash 使用 position-independent A/B：

- A / canonical：`primary_cp_app=1360 KiB` 加 `primary_ap_app=2380 KiB`，合计 3740 KiB。
- B / trial：`s_app=3740 KiB`。
- 必要固定区保留 `ota_fina_executive=4 KiB`、`usr_config=128 KiB`、`flashdb=128 KiB`、`coredump=360 KiB`，以及 SDK 使用的尾部分区。
- `/dl` 和 `/data` 位于 SD 卡 FATFS 的 `h2loader/dl` 和 `h2loader/data`，不占用内部 Flash。

分区总长度正好为 8 MiB。A/B 边界变化时，旧布局设备必须按 board recovery 流程重新烧录 combined image，不能直接把新 package 当作普通 self-update。

## 平台配置

CP 独占 UART0 RX 和 physical TX serializer，managed UART 固定为 230400 8N1；Host 的默认值与固件一致，显式 `--baud` 只用于连接另行配置的镜像。CP 不解析 H2IKCP 或 H2Loader command；AP 通过 mailbox-backed UART PAL 持有 IO Stream iKCP session 和 Loader/App command owner。Loader 只有在 firmware identity 与共享 Loader state 初始化成功后才确认 UART session；随后在 storage mount、publish recovery 和 startup retry 之前启动 UART command task。startup 与 UART/BLE lifecycle/package operation 继续由共享 mutex 串行化，因此 mount 或启动恢复失败时仍保留串口诊断与管理入口。

Host 打开 managed serial 后、借出 stream 前明确 deassert DTR/RTS；不支持控制线的 endpoint 只有返回 canonical `UNSUPPORTED` 才可继续，其它错误立即终止连接。APP 与 Loader status 都从设备端同一个 BLE public/identity MAC 返回 12 位小写十六进制 `device_uid`，BLE 重启后以该 UID 验证设备，而不是信任 endpoint/address。

## 预期表现

运行 `bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- scan` 后，只选择结构化 identity 为 `board=bk7258_v3_202405`、`target=bk7258`、`active_role=loader`、`transport=iostreamikcp` 的设备。APP 或 Loader 状态都可以通过 `send --file <build-dir>/update.tar.zlib` 直接发布 Stage；安装使用 `reboot upgrade`。

验收必须看到 Partition 2 候选 Loader 启动、自动回写、最终运行 Partition 1，且 Partition 1/2 metadata valid、image checksum 相同、Stage 已清理，并在 power-cycle 后复查。reboot accepted 本身不是完成。已经安装 H2Loader 的正常路径不调用 `bk_loader`；只有 scan、status、`reboot loader` 都无法通信或 Loader 无法自我恢复时，才按 `bk_loader.json` 烧录 combined image，并把它记录为恢复路径。不支持 reliable command contract 的旧 image 只能进入该 recovery 流程，不能使用 legacy raw H2Loader command 迁移。

PR #64 当前源码已经分别构建 Loader 与 APP package。BK7258 V3 QFN88 实板已不可用，
因此 UART/BLE、APP/Loader 和升级/copy-back 的物理矩阵是明确 deferred gate；构建结果、ESP
结果或 CI 都不能替代 BK 硬件 PASS。获得替换板后应使用同一个 runtime-flag e2e-runner 补齐
报告，不为 BK 另写 compile-time 设备参数。
