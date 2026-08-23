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

先用 `bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- scan` 选择返回 `board=szp`、`target=esp32s3` 的设备。安装 Bazel target 生成的标准 package，要求 marker 为 `app=v1 data=v1`，App 与 data 进度均报告 installed，并进入 pending-confirm。完成 confirmation 后重新安装完全相同的 package，要求 App 与 data 都报告 `detail=unchanged`，不出现 App writer/erase 或 data clear/write，最终状态保持 `confirmed`。

每一步都保存 package 中的 `manifest.image_sha256`、顶层 `checksum`、设备 `H2_LOADER_INSTALL_PROGRESS` 和最终 `H2_LOADER_STATUS`。`phase=2` 是 App，`phase=3` 是 data。不能只用重启后 App 能运行作为通过依据。

若先清除 H2Loader Preference 中的 installed/confirmed 状态，再安装 App 内容相同的完整包，App partition 仍不得擦写，但启动必须重新进入 pending-confirm。这一步验证 Preference 只负责确认安全门，不负责判断内容是否变化。
