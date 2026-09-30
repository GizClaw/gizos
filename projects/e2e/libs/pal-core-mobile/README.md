# Mobile PAL Core qualification

两个原生 App 运行 `projects/e2e/apps/pal-core` 的同一份 Core v2 C 测试： 41 个必选 case、9 个 Core vtable、46 个操作。测试线程在后台运行，UI 仅显示结果。 这证明 Core（Memory、Log、Time、Timer、Task、Queue、Sync、SystemEvent、FirmwareInfo） 契约；Display、Audio、BLE、网络等能力仍需各自的 E2E。

## 实现和产物边界

- iOS/Android provider 保留平台 UI/媒体实现；Core 的线程、同步、队列和定时器 复用 `libs/pal/providers/posix/pal_core`。Web pthread provider 也使用相同线程核心。
- PAL Task 使用真正的 pthread，并分配实际工作栈。Android 为 Bionic 的线程元数据 额外预留空间，`min_stack_size` 表示至少可用的栈空间。
- Wall Time 默认读取系统时间；`set_wall_ms` 设置进程内 PAL 时钟偏移，不修改手机系统时间。
- FirmwareInfo 来自 iOS App 的 `CFBundleShortVersionString`、Android 的 `PackageInfo.versionName`；Android 宿主须在启动前调用 image-version setter。
- Log observer 检查真实 stderr/liblog 输出；Task observer 从 pthread 查询栈并检查 局部变量地址；故障注入作用在真实 task-stack 分配；资源计数来自实际对象生命周期。
- `libs/app_host` 负责组装 Runtime。Runtime 和 Atomic 不归入 PAL 包。
- `h2_swift_package` / `h2_android_aar` 位于 `tools/bazel/mobile_package.bzl`， 只构建本地产物，不上传或发布。
- Swift Package 包含 iOS arm64 真机和 arm64 模拟器静态 XCFramework、完整 C 头文件、 module map、`Package.swift`、SHA-256 和 metadata。最低 iOS 16，不含 Intel 模拟器。
- AAR 包含 arm64-v8a 原生库、Prefab 头文件/元数据，附带 Maven POM、SHA-256。 最低 API 和 NDK 版本从实际 ELF note 读取；本次为 API 31 / NDK 28c， E2E APK 最低 API 31。日志观察器使用 API 30 起提供的 liblog hook。 C++ runtime 静态链接，APK 不依赖额外的 `libc++_shared.so`。
- 两个 App 通过 `:packaged` 从最终 XCFramework ZIP/AAR 提取 PAL 二进制和头文件， 不再次编译 PAL provider。Android runner 还比较 APK 内 `.so` 与 AAR 的内容一致。

## 构建

```sh
export ANDROID_HOME="$HOME/Library/Android/sdk"
export ANDROID_NDK_HOME="$ANDROID_HOME/ndk/28.2.13676358"
export JAVA_HOME='/Applications/Android Studio.app/Contents/jbr/Contents/Home'

bazel build --config=ios_sim_arm64 \
  //libs/pal/providers/ios/pal_core:swift_package \
  //projects/e2e/targets/ios_application/pal-core:pal_core_e2e

bazel build --config=android_arm64 \
  //libs/pal/providers/android/pal_core:aar \
  //projects/e2e/targets/android_binary/pal-core:pal_core_e2e
```

构建需要 Xcode/iOS SDK、Android SDK/NDK 和 Java。保留已配置的 Bazel disk cache。 包只是本地发布候选产物；真机切片交叉编译通过不等于真机 E2E 通过。

## 模拟器资格测试

先启动专用测试模拟器并显式指定目标，避免误操作其他设备。测试只安装/启动自己的 App。

```sh
export H2_IOS_SIMULATOR_UDID='<booted-simulator-uuid>'
make bazel-test-ios_pal_core_simulator_test

export H2_ANDROID_SERIAL='emulator-5580'
make bazel-test-android_pal_core_simulator_test
```

两个 Make 入口分别调用同名 `manual` Bazel `py_test`，并禁用 live test result cache。 共享执行器和接入约定见 [mobile_e2e.md](../../../../tools/bazel/mobile_e2e.md)。 所有 suite 共用 `tools/bazel/mobile_e2e.py` 的参数解析、事务和结果保存入口；Core 的 `run_mobile.py` 只负责运行与 registry/PASS 断言。 每轮先终止旧 App、安装当前构建、删除旧结果，再启动；公共 runner 限制启动和报告等待共 90 秒。 超时、缺少 case、FAIL、BLOCKED、NOT_RUN、cleanup/teardown 失败和资源不平衡均失败。 结束后终止测试 App；不关闭其他 App 或模拟器。

测试输出目录包含 `qualified.json`、`environment.json` 和原生日志。 环境记录包含模拟器身份、App 与 SDK 包 SHA-256。仓库 evidence 保存实际运行结果。 本轮环境为 iOS 26.5 / iPhone 17 Pro、Android API 36 / arm64 ATD。

Swift Package 的独立 Swift 消费者检查（编译并链接一个动态库，不执行 iOS 真机测试）：

```sh
python3 projects/e2e/libs/pal-core-mobile/check_swift_package.py \
  bazel-bin/libs/pal/providers/ios/pal_core/swift_package.package
```
