# PAL JSON E2E

独立 portable C App 对 `h2_pal_json_api_t` 的 24 个 public vtable 操作运行同一份 mandatory case registry。`app/tests/test_interface_coverage.py` 从公开 header 自动枚举操作，并检查每个操作有 wrapper 调用和完整 provider vtable preflight；增加或删除操作时，inventory 会要求同步更新 App。`qualification.json` 保存实际平台执行证据及源码、产物摘要。构建成功或 yyjson provider unit test 不能替代六端 App 运行。

15 个 case 覆盖严格 JSON/UTF-8 解析、decoded 字符串 span、object/array 查询与构造、修改和 roundtrip、值类型、非法输入、bytes/depth/value 上限、跨文档 attachment、cycle、不可变文档、替换后的 stale handle、错误输出清零、serialize 的 NUL/长度与 release。最后两个 case 通过 launcher 提供的 portable probe callback，在真实 yyjson provider 上验证 live document/buffer 时拒绝 destroy、完整释放，以及逐点 allocator 失败后的零残留。App 只 include public JSON PAL 和 Runtime header；yyjson 的创建、选择和销毁都留在 launcher/provider probe。JSON 测试不需要网络 fixture 或 Wi-Fi 凭据。

资格要求每个平台完全相同的 registry ID 顺序全部 PASS，`failed=blocked=not_run=0`、`complete=qualified=1`，并记录 provider/Runtime teardown。macOS 是独立原生进程；WASM 必须在真实 Chromium Worker 执行；iOS 和 Android 安装各自实际 IPA/APK，runner 从 Swift Package/XCFramework 与 AAR 导出的 provider 符号运行，并核对打包二进制。模拟器不代表手机实机。

```sh
bazel test --lockfile_mode=off \
  //projects/e2e/apps/pal-json/app:interface_coverage_test \
  //projects/e2e/targets/cc_binary/pal-json:desktop_test \
  //projects/e2e/targets/pkg_tar/pal-json:browser_test
H2_IOS_SIMULATOR_UDID=<booted-uuid> scripts/common/bazel_manual_test.py \
  //projects/e2e/targets/ios_application/pal-json:ios_pal_json_simulator_test \
  --config=ios_sim_arm64 --lockfile_mode=off
ANDROID_HOME=<sdk> ANDROID_NDK_HOME=<ndk> H2_ANDROID_SERIAL=<emulator-serial> \
  scripts/common/bazel_manual_test.py \
  //projects/e2e/targets/android_binary/pal-json:android_pal_json_simulator_test \
  --config=android_arm64 --lockfile_mode=off
```

DevKit 和 BK7258 使用原有 H2Loader、P1 和 coredump，构建 managed App update package 后先查询 UID、Loader/P1、Stage、App/P2 和 coredump；`send --file` 后核对 Stage 中 package/image checksum，再 `reboot upgrade --monitor` 安装。首次运行须看到新镜像的 `H2_JSON_BOOT`、完整 case ledger、`H2_JSON_READY rc=0 confirm=0`。随后独立 `reboot app --monitor` 做正常重启复验；monitor 重连产生的 ledger replay 不是新的一次执行。两个 boot 的完整结果都通过后，最终查询 P1、P2、Stage empty、last_result=0 与 coredump 基线。原始串口 log 与二进制 dump 保存在本地，结构化 receipt 和各 log SHA 写入 evidence。

```sh
# 先 source 仓库固定的 firmware-devenv/export.sh；BK7258_PATH 须指向干净的 3.1.1 SDK checkout。
bazel build --config=esp32s3 --lockfile_mode=off \
  --//tools/bazel:firmware_version=pal-json-esp-<unique-version> \
  //projects/e2e/targets/h2loader_tar_zlib/pal-json/devkit:package
bazel build --config=bk7258 --lockfile_mode=off \
  --//tools/bazel:firmware_version=pal-json-bk-<unique-version> \
  //projects/e2e/targets/h2loader_tar_zlib/pal-json/bk7258_v3_202405:package
```

本次 [结构化资格记录](qualification.json) 绑定了当前 public header、portable App、provider probe、移动平台包装层的源码 SHA-256，及各端实际 artifact、SDK 包、Bazel test log 和设备串口 log 的摘要。各块板的 boot ID 是宿主根据独立 reboot 命令与原始 log 摘要分配的观测 ID，不冒充设备内部随机 run ID。资格或 provider teardown 失败时，launcher 留下明确失败标记并保持 App 未确认；只有完整通过且 H2Loader 确认成功后才输出 `H2_JSON_READY rc=0 confirm=0`。

| 平台 | 本次实测 |
| --- | --- |
| macOS 原生进程 | 15/15 PASS，provider teardown=0 |
| 真实 Chromium Worker / WASM | 15/15 PASS，provider 与 Web teardown=0 |
| iOS 26.5 Simulator / Swift Package | 15/15 PASS，XCFramework provider 符号在 IPA 中可见，teardown=0 |
| Android API 36 arm64 Emulator / AAR | 15/15 PASS，APK 与 AAR 的 `.so` 逐字节相同、工厂和 provider 符号公开，teardown=0 |
| ESP32-S3 DevKit | 安装 boot 与独立正常 reboot 各 15/15 PASS；P1 保留、Stage empty、coredump blank |
| BK7258 AP | 安装 boot 及两次显式正常 reboot 各 15/15 PASS；P1 保留、Stage empty、原 32-byte coredump 前后相同 |

BK7258 的逐点 allocator failure case 在该板上明显慢于其他端，整轮同步测试约需三分钟；这次资格只证明功能与清理，不声明性能。未来修改上述源码或 SDK 后，历史 receipt 不会自动证明新产物通过，必须重跑并更新摘要。
