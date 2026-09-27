# PAL Storage E2E

独立验证 FileSystem 的 11 个 vtable 操作和 Preferences 的 17 个操作（包括 namespace 上的 16 个方法），共 28 项。Disk 的六个接口操作裸分区擦写，单独归入后续 `pal-disk`；本 App 不擦写真实固件分区。

Portable App 只借用 Runtime 中的 FS、Preferences 和 Memory，以及宿主独占的测试路径/namespace。宿主负责建立隔离的数据目录、真实 provider、进程重启和结果汇总。禁止把旧的混合 PAL App 当作本 App 的测试实现或把缺失接口当作通过。

验收包含二进制/空文件、短读和 EOF、seek、truncate、rename/remove、递归 clear、相邻路径隔离、反复打开关闭；Preferences 包含五种值类型、覆写、类型错配、namespace 隔离、只读保护、迭代/提前关闭、分配失败及 commit。持久化验收分 seed/verify 两个独立进程，第二个进程必须用全新 provider 重新读取第一阶段提交的数据；关闭再打开一个 handle 不能代替重启验证。

只有全部必选 case PASS、两个阶段身份一致、全部 handle/cursor 回收且测试数据清理成功才允许 qualified。平台不支持某个操作时保持 BLOCKED，不能从清单移除。断电原子性、恶意宿主并发修改和物理介质寿命不由正常进程重启证据证明。

验收平台为 macOS、WASM、iOS、Android、ESP32-S3 DevKit 和 BK7258。macOS/移动端使用真实 OS FileSystem 和 SQLite Preferences；Web 使用 IDBFS 与 localStorage；DevKit 使用 LittleFS；BK 使用 SD 卡 FATFS 和 FlashDB。每端接入同一 portable 契约，结果逐平台记录；未运行的平台不宣称通过。

硬件测试只使用 `/data/pal-storage`、`h2storea`、`h2storeb` 和阶段控制 namespace `h2storectl`。控制信息绑定实际镜像版本和 nonce；seed 完成后写入下一阶段，宿主正常重启 App，再读取 verify 结果。两次 boot 的记录必须完整、nonce/镜像一致。重放日志不算第二次执行；通过后的再次 boot 只报告 already-complete，不重复生成通过证据。

## 运行入口

```sh
bazel test //projects/e2e/apps/pal-storage/app:interface_coverage_test \
  //projects/e2e/targets/cc_binary/pal-storage:desktop_test \
  //projects/e2e/targets/pkg_tar/pal-storage:browser_test

H2_IOS_SIMULATOR_UDID=<booted-uuid> make bazel-test-ios_pal_storage_simulator_test
H2_ANDROID_SERIAL=emulator-5580 make bazel-test-android_pal_storage_simulator_test
```

移动端需要当前 Xcode/iOS SDK 或已配置的 Android SDK/NDK/Java，模拟器须已启动。App 包从 SDK 档案导入 PAL；Android runner 会核对 APK 内的 `.so` 与 App 原生配置实际选择的 AAR 字节一致。结果位于 Bazel test outputs 的 `qualified.json`，两个 phase 的 PID 必须不同。

硬件由 `projects/e2e/targets/h2loader_tar_zlib/pal-storage/devkit:package` 和 `projects/e2e/targets/h2loader_tar_zlib/pal-storage/bk7258_v3_202405:package` 构建。每次设置独立 firmware version，使用 H2Loader managed serial send，核对 Stage 的包/镜像 SHA 后 `reboot upgrade --monitor` 收集 seed；再 `reboot app --monitor` 收集 verify。原始串口日志可能含重放，只接受版本/nonce/phase 一致的完整唯一 case ledger。最后确认 Stage 为空、P2 App 有效、Loader 保留且 coredump 与原基线一致。


## 实测结果

2026-09-28（Asia/Singapore）六个平台全部完成：macOS、WASM/Chromium、iOS Simulator、Android Emulator、ESP32-S3 DevKit、BK7258。每端均为 30 PASS、0 FAIL、0 BLOCKED，正常进程/浏览器/设备重启后的文件和 Preferences 持久化、测试数据清理均通过。汇总与逐平台记录链接在 [qualification.json](qualification.json)。iOS/Android 的证据来自模拟器，不声称物理手机已验证。

DevKit 实测镜像为 `pal-storage-esp-20260928-r1`，全部测试写入板载 Flash，无需 SD 卡。BK 为 `pal-storage-bk-20260928-r2`，文件位于 SD 卡、Preferences 位于 FlashDB。BK 的 seed 超过旧 120 秒诊断提示，且 CLI 的 install/reboot 监视先达到宿主期限；重新连接后从同一 boot 的不可变 replay 收齐结果。设备最终两阶段均成功、rc/control/cleanup 均为零。源码把后续运行的诊断预算提高为 600 秒；该预算调整本身不伪装为新的硬件运行，证据仍绑定 r2 的实际镜像 SHA。
