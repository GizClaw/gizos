# BK7258 V3 202405 H2Loader

## 构建

```sh
bazel build --config=bk7258 \
  --//tools/bazel:firmware_version=<version> \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/bk7258_v3_202405:package
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

CP 独占 UART0 RX 和 physical TX serializer，UART 固定为 230400 8N1。CP 不解析 H2IKCP 或 H2Loader command；AP 通过 mailbox-backed UART PAL 持有 IO Stream iKCP session 和 Loader/App command owner。Loader 只有在 firmware identity 与共享 Loader state 初始化成功后才确认 UART session；随后在 storage mount、publish recovery 和 startup retry 之前启动 UART command task。startup 与 UART/BLE lifecycle/package operation 继续由共享 mutex 串行化，因此 mount 或启动恢复失败时仍保留串口诊断与管理入口。

## 预期表现

运行 `bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- scan` 后，只选择结构化 identity 为 `board=bk7258_v3_202405`、`target=bk7258`、`active_role=h2loader`、`transport=iostreamikcp` 的设备。用 scan 返回的 port 执行默认 `status`，确认 `upgrade_phase=idle`；再通过默认 `send --file <build-dir>/update.tar.zlib` 在同一 reliable session stage package。App image 需要更新时先执行 `rollback`，重新连接并确认 Loader role 后再发送 package。

验收必须看到 AP/CP transition、trial 与 canonical 启动、最终 `active_version=<version>`、canonical running partition、`upgrade_phase=idle` 和 power-cycle 后复查。`H2_LOADER_UPGRADE result=OK` 本身不是完成。已经安装 H2Loader 的正常路径不调用 `bk_loader`；只有 scan、status、reboot-loader 都无法通信或 Loader 无法自我恢复时，才按 `bk_loader.json` 烧录 combined image，并把它记录为恢复路径。不支持 reliable command contract 的旧 image 只能进入该 recovery 流程，不能使用 legacy raw H2Loader command 迁移。
