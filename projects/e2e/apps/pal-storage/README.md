# PAL Storage E2E

独立验证 FileSystem 的 11 个 vtable 操作和 Preferences 的 17 个操作（包括 namespace 上的 16 个方法），共 28 项。Disk 的六个接口操作裸分区擦写，单独归入后续 `pal-disk`；本 App 不擦写真实固件分区。

Portable App 只借用 Runtime 中的 FS、Preferences 和 Memory，以及宿主独占的测试路径/namespace。宿主负责建立隔离的数据目录、真实 provider、进程重启和结果汇总。禁止把旧的混合 PAL App 当作本 App 的测试实现或把缺失接口当作通过。

验收包含二进制/空文件、短读和 EOF、seek、truncate、rename/remove、递归 clear、相邻路径隔离、反复打开关闭；Preferences 包含五种值类型、覆写、类型错配、namespace 隔离、全部写方法的只读保护、迭代/提前关闭、分配失败及 commit。契约 2 共 36 个必选 case（seed 31、verify 3、clean-verify 2）：Blob 验证 1/255/256/1537/16383/16384 字节，字符串验证 1/255/256/4095 字节和 UTF-8；整数验证完整零值与极值。保留 100 次 commit/close/reopen 循环，另外连续覆盖同一个键 1000 次并验证最终值 999、精确键数和相邻 namespace 未改变。

持久化验收使用三个独立进程或 boot：seed 提交 16 KiB Blob 和覆盖写结果，verify 用全新 provider 读回全部数据，然后在 namespace A 逐键 remove+commit、namespace B clear+commit 并删除专用 FS 路径；clean-verify 再次用全新 provider 确认两个 namespace 迭代为空、所有测试键 NOT_FOUND、FS 路径不存在。宿主最后重复一次 clean-verify，以确认完成状态后再次启动仍为空；重复结果单列，不增加唯一 case 通过数。关闭再打开一个 handle 不能代替重启验证。

只有全部必选 case PASS、各次启动身份一致、全部 handle/cursor 回收且重启后的测试数据清理确认成功才允许 qualified。平台不支持某个操作时保持 BLOCKED，不能从清单移除。断电原子性、恶意宿主并发修改和物理介质寿命不由正常进程重启证据证明。

验收平台为 macOS、WASM、iOS、Android、ESP32-S3 DevKit 和 BK7258。macOS/移动端使用真实 OS FileSystem 和 SQLite Preferences；Web 使用 IDBFS 与 localStorage；DevKit 使用 LittleFS；BK 使用 SD 卡 FATFS 和 FlashDB。每端接入同一 portable 契约，结果逐平台记录；未运行的平台不宣称通过。

硬件测试只使用 `/data/pal-storage`、`h2storea`、`h2storeb` 和阶段控制 namespace `h2storectl`。控制信息绑定实际镜像版本和 nonce；seed 完成后写入下一阶段，宿主正常重启 App，再读取 verify 结果。三个阶段的 boot 记录必须完整、contract/nonce/镜像一致。阶段控制同时绑定 contract version，旧契约阶段不能直接作为新契约完成状态。重放日志不算新的执行；通过后的再次 boot 重新检查清理状态并报告 already-complete 的 empty/rc，不重复生成通过证据。

## 运行入口

```sh
bazel test //projects/e2e/apps/pal-storage/app:interface_coverage_test \
  //projects/e2e/targets/cc_binary/pal-storage:desktop_test \
  //projects/e2e/targets/cc_binary/pal-storage:cleanup_persistence_test \
  //projects/e2e/targets/cc_binary/pal-storage:device_lifecycle_test \
  //projects/e2e/targets/pkg_tar/pal-storage:browser_test

H2_IOS_SIMULATOR_UDID=<booted-uuid> bazel test --config=ios_sim_arm64 --cache_test_results=no \
  //projects/e2e/targets/ios_application/pal-storage:ios_pal_storage_simulator_test
H2_ANDROID_SERIAL=emulator-5580 bazel test --config=android_arm64 --cache_test_results=no \
  //projects/e2e/targets/android_binary/pal-storage:android_pal_storage_simulator_test
```

移动端需要当前 Xcode/iOS SDK 或已配置的 Android SDK/NDK/Java，模拟器须已启动。App 包从 SDK 档案导入 PAL；Android runner 会核对 APK 内的 `.so` 与 App 原生配置实际选择的 AAR 字节一致。结果位于 Bazel test outputs 的 `qualified.json`，seed/verify/clean-verify/重复 clean-verify 四次启动的 PID 必须各不相同。Desktop 的 `cleanup_persistence_test` 在真实 SQLite/FS 中重新加入遗留数据，验证第三阶段确实拒绝 remove、clear 或 FS cleanup 未持久生效的状态。`device_lifecycle_test` 使用真实桌面 provider 运行设备阶段控制，检查 1/2/3/4/4 的独立进程、不可变 ledger 重放、完成状态重检以及遗留数据拒绝；它验证共享控制逻辑，不作为 FlashDB/LittleFS 实板资格。

Desktop 三个生命周期测试的每进程预算为 180 秒，整个 Bazel test 使用 long timeout；大值与真实同步/重新打开压力必须在有限预算内完成。外部设备与已启动模拟器的结果缓存关闭，构建缓存保持启用。

硬件由 `projects/e2e/targets/h2loader_tar_zlib/pal-storage/devkit:package` 和 `projects/e2e/targets/h2loader_tar_zlib/pal-storage/bk7258_v3_202405:package` 构建。每次设置独立 firmware version，使用 H2Loader managed serial send，核对 Stage 的包/镜像 SHA 后 `reboot upgrade --monitor` 收集 seed；再 `reboot app --monitor` 收集 verify，再次正常重启收集 clean-verify，最后重启确认 already-complete empty=1 rc=0。原始串口日志可能含重放，只接受版本/nonce/phase 一致的完整唯一 case ledger。最后确认 Stage 为空、P2 App 有效、Loader 保留且 coredump 与原基线一致。


## 资格边界

契约 2 的当前记录见 [qualification-v2.json](qualification-v2.json)：macOS、WASM/Chromium、iOS Simulator 和 Android Emulator 均完成 36 PASS、清理后的两次独立重新启动检查；各 receipt 绑定自己实际执行的源码 revision 与 artifact SHA，不把后来修复或 metadata 更新重绑到早先记录。iOS 同时核对 XCFramework 与 IPA 的 Storage provider 导出符号及 SDK/可执行文件哈希；Android 核对 APK 的 provider 与实际 AAR 同字节、四个公开 Storage 符号及两个 archive 哈希。ESP32-S3 DevKit 和 BK7258 的既有包保留原来的构建 source/image 身份，两块实板的契约 2 执行资格仍待取得。

当前设备资格采集使用 1/2/3/4/4 五次独立 boot，最后两次都重新确认完成状态为空，且不产生新 case。完整 ledger、不可变 replay 及实际 UID/镜像/Loader/coredump 的前后核对方式见 [device verifier](../../libs/pal-storage-device/README.md)；验证器不替代宿主真实 reboot receipts。

契约 2 的 36 case 必须重新取得各平台当前 source/artifact 对应的执行结果。下方以及已提交的 qualification JSON 是契约 1、30 case 的历史证据，不能证明新增 16 KiB、1000 次覆盖写或清理后重启强度已在六端通过。BK 当前不支持空 Blob 和空字符串，而其他 provider 支持；公共 Pref contract 没有统一空值保证，本套件不把此类差异补写成跨平台承诺。正常进程重启也不证明掉电恢复、跨键事务原子性或并发可见性。

## 历史实测结果（契约 1）

2026-09-28（Asia/Singapore）六个平台全部完成：macOS、WASM/Chromium、iOS Simulator、Android Emulator、ESP32-S3 DevKit、BK7258。每端均为 30 PASS、0 FAIL、0 BLOCKED，正常进程/浏览器/设备重启后的文件和 Preferences 持久化、测试数据清理均通过。汇总与逐平台记录链接在 [qualification.json](qualification.json)。iOS/Android 的证据来自模拟器，不声称物理手机已验证。

DevKit 实测镜像为 `pal-storage-esp-20260928-r1`，全部测试写入板载 Flash，无需 SD 卡。BK 修复后的实测镜像为 `pal-storage-bk-20260928-r3`，文件位于 SD 卡、Preferences 位于 FlashDB。该镜像实际使用 600 秒诊断预算并完成 seed 27 PASS 与正常重启后的 verify 3 PASS；首个 install monitor 达到宿主期限后，通过重新连接收集同一次 boot 的不可变结果，rc/control/cleanup 均为零。最终 App 已确认，Stage 为空，原 Loader 和 coredump 保留。

FlashDB 的保留 namespace 和写入错误路径另由 `//native_component_src/bk7258/ap/h2_pal_core:pref_flashdb_test` 验证：真实 provider 接入可注入提交前/后错误的 SDK 边界，检查值/类型一致、旧格式读取、分配/读取错误及孤立元数据回收；同一测试通过 ASan/UBSan。实板用例不宣称注入了物理 Flash 故障。
