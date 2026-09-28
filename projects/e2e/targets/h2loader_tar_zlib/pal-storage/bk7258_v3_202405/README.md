# bk7258_v3_202405 PAL Storage E2E

使用 已插入的 SD 卡 FATFS（FileSystem）和板载 FlashDB（Preferences），执行同一份 28 操作/30 case portable Storage 契约。测试只拥有 `/data/pal-storage` 目录及 `h2storea`、`h2storeb`、`h2storectl` namespaces，不格式化分区或 SD 卡。DevKit 不需要 SD 卡。

```sh
bazel build --config=bk7258 --lockfile_mode=off \
  --//tools/bazel:firmware_version=pal-storage-<unique-version> \
  //projects/e2e/targets/h2loader_tar_zlib/pal-storage/bk7258_v3_202405:package
```

SDK/toolchain 必须使用仓库固定版本，并保留 Bazel/native ccache。此 fixture 的 UID 为 `c8478ca2a87c`，串口为 `/dev/cu.usbserial-20131240`；重新安装前必须查询 live status 确認设备身份。通过 H2Loader send 传输 `update.tar.zlib`，校验 Stage package SHA/镜像 SHA/版本，再执行 `reboot upgrade --monitor`。BK 的安装写入比 seed case 更慢，允许整个 managed install 完成，不能在写入期间 reset。

seed boot 必须产生 phase=1 的 27 个唯一 PASS case，rc/control/cleanup 均为零。宿主随后 `reboot app --monitor`，verify boot 必须产生 phase=2 的 3 个唯一 PASS case；两次报告 nonce 和实际镜像版本必须相同。确认 App 与通过测试是两个独立结果。设备保留串口命令服务并慢速重放当前 boot 的不可变结果，重放不算新的运行。

最终核验 active version/checksum、P2 validity、Stage empty 和原 Loader P1 identity；对照安装前 coredump 基线。结构化 build/qualification 证据保存在 `evidence/`；原始串口日志保留为本地诊断产物。

BK 的 100 次真实 FlashDB 写入/重新打开循环需要较长时间，诊断 watchdog 预算为 600 秒。r2 实测使用旧的 120 秒提示，仍完成全部 seed case；实际报告和该提示单独记录，旧提示不伪装成新的运行。
