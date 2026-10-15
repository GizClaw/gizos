# devkit PAL Storage E2E

使用 板载 Flash LittleFS（FileSystem）和独立 pref LittleFS（Preferences），执行同一份 28 操作/36 case portable Storage 契约 2。测试只拥有 `/data/pal-storage` 目录及 `h2storea`、`h2storeb`、`h2storectl` namespaces，不格式化分区或 SD 卡。DevKit 不需要 SD 卡。

```sh
bazel build --config=esp32s3 --lockfile_mode=off \
  --//tools/bazel:firmware_version=pal-storage-<unique-version> \
  //projects/e2e/targets/h2loader_tar_zlib/pal-storage/devkit:package
```

SDK/toolchain 必须使用仓库固定版本，并保留 Bazel/native ccache。此 fixture 的 UID 为 `9888e0115c52`，串口为 `/dev/cu.usbmodem201313231`；重新安装前必须查询 live status 确認设备身份。通过 H2Loader send 传输 `update.tar.zlib`，校验 Stage package SHA/镜像 SHA/版本，再执行 `reboot upgrade --monitor`。BK 的安装写入比 seed case 更慢，允许整个 managed install 完成，不能在写入期间 reset。

seed boot 必须产生 phase=1 的 31 个唯一 PASS case，rc/control/cleanup 均为零。宿主随后 `reboot app --monitor`，verify boot 必须产生 phase=2 的 3 个唯一 PASS case；再次正常重启，clean-verify boot 必须产生 phase=3 的 2 个唯一 PASS case，并证明 remove/clear 已持久生效。最后再重启，应报告 already-complete empty=1 rc=0。各次报告 contract=2、nonce 和实际镜像版本必须相同。确认 App 与通过测试是两个独立结果。设备保留串口命令服务并慢速重放当前 boot 的不可变结果，重放不算新的运行。

最终核验 active version/checksum、P2 validity、Stage empty 和原 Loader P1 identity；对照安装前 coredump 基线。结构化 build/qualification 证据保存在 `evidence/`；原始串口日志保留为本地诊断产物。

契约 2 的 16 KiB Blob、1000 次连续覆盖写和清理后重启需要重新实板资格；已提交的 30 case 记录保持为契约 1 历史证据。
