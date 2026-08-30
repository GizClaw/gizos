# SZP Partial Update

`partial-update` Example 提供 App generation 和 `/data/version.txt` data generation observation；本页的外部验收流程使用它验证 H2Loader 从完整 format 1 package 独立跳过未变化的 App 与 data。验收 marker 形如：

```text
H2_PARTIAL_UPDATE_SMOKE result=PASS app=v2 data=v1
```

## 构建

在 ESP-IDF 环境中构建 canonical Bazel firmware target：

```sh
bazel build --config=esp32s3  //projects/example/targets/h2loader_tar_zlib/partial-update/szp:package
```

该 target 使用固定的 App generation 与 data fixture，和其它 firmware entry 一样只生成一个标准 `<board>-<image>-<target>.update.tar.zlib`。包同时包含 App image 与显式 data snapshot，不生成 app-only、data-only、按 generation 命名的额外 package 或兼容入口。ELF、map、flash image 与 metadata 仍由同一 target 作为 native outputs 提供。

## 设备验收

先用 `bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- scan` 选择返回 `board=szp`、`target=esp32s3` 的设备。通过 `send` Stage Bazel target 生成的标准 package，再执行 `reboot upgrade`。要求 marker 为 `app=v1 data=v1`，App 与 data 的瞬时安装进度均完成；最终 authoritative status 必须运行在 Partition 2 APP、`partition_2.valid=true` 且 identity 与 package 一致，并且 `stage.valid=false`。重新 Stage 完全相同的 package 时，要求 App 与 data 都报告 `detail=unchanged`，不出现 App writer/erase 或 data clear/write，最终仍由相同 Partition 2 identity 完成 Stage 收尾。

每一步都保存 package 中的 `manifest.image_sha256`、顶层 `checksum`、设备瞬时安装进度和最终 `H2_LOADER_STATUS`。进度中的 image 与 data phase 只描述当前 I/O，不是持久化生命周期状态。不能只用重启后 App 能运行作为通过依据。

若先清除 `partition_2` metadata，再 Stage App 内容相同的完整包，启动时必须从当前固件 identity 与实际 checksum 重新建立可信的 Partition 2 记录；内容未变化时 App partition 仍不得擦写。流程不得重建 installed/confirmed phase。
