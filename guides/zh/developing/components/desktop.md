# Desktop Components

`libs/pal/providers/desktop/` 提供 Desktop target 的 PAL adapter、host package integration 和验证入口。每个 project 在自己的 `cc_binary` 或 `macos_application` rule root 保存最终入口与 app-specific 配置。两者让跨平台 app 与 Libraries 在 macOS、Linux 或其他 host 环境中使用与设备端相同的 contract，而不把 pthread、SDL、PortAudio、wolfSSL 或 host filesystem 泄漏到跨平台代码。

## 目录结构

目录结构是：

```text
libs/pal/providers/desktop/
├── BUILD.bazel                     # Desktop aggregate build
├── pal_core/
│   ├── BUILD.bazel                 # 跨 Desktop core/simulator PAL package
│   ├── include/
│   ├── src/
│   └── tests/
├── app_support/                    # OS/provider selection 与 Runtime composition
└── tests/                          # 只保留跨 package Desktop integration
```

Desktop 的 semantic target 是 `//libs/pal/providers/desktop/pal_core:pal_core`；顶层 `BUILD.bazel` 只保存跨 package integration test，不是第二个 semantic owner。`pal_core/` 只包含跨 Linux/macOS 的 standard C/C++ core 与确定性 simulator；`app_support/` 选择真实 Linux/Darwin owner 与可复用 provider，并负责 consumer-first teardown。Windows 的 Memory、Time、Task、Queue、Sync、Filesystem、Net、Netif 和 System Event 等 OS capability 由 `//libs/pal/providers/windows/pal_core:pal_core` 独立拥有，不能反向依赖 Desktop；共享的 `//projects/e2e/targets/cc_binary/pal:pal_e2e_test` 只复用 OS-neutral case registry。SDL3、PortAudio、FFmpeg 与 SQLite 分别由 `libs/pal/providers/<provider>/` 拥有；LVGL Display adapter 由 `libs/lvgl` 拥有。Opus 没有 GizOS adapter，Desktop、H106 与 Audio System consumer 直接依赖 `//tools/bazel:vendor_third_party_opus`，不保留 implementation-free 的 `desktop/pkgs/opus` forwarding package。PortAudio 直接依赖完整的 `@h2_vendor_speexdsp//:speexdsp`，不保留 implementation-free 的 `libs/speexdsp` 或 Desktop forwarding package。

Desktop executable 归 App owner project：

```text
libs/pal/providers/desktop/
└── app_support/                    # Cross-project Runtime composition

tools/bazel/desktop_layout/         # Strict JSON layout generator/validator

projects/<owner>/
├── libs/desktop/<component>/       # Project-local Bazel Desktop glue，可选
└── targets/
    ├── cc_binary/<app>/            # Host executable、main 与 layout
    └── macos_application/<app>/    # macOS/rules_apple bundle，可选
```

例如 Example Display 位于 `projects/example/targets/cc_binary/display/`。H106 Main 使用一个 `projects/h106/targets/cc_binary/main/` package 和共享 `main.cpp`，由 `:h106-tiga` 与 `:h106-zero` 两个 Bazel target 设置产品 defines；不能复制两个 source entry。group 内多个 entry 共用的 Desktop host assembly 属于该 group 的 target component，例如 H106 使用 `projects/h106/libs/desktop/main/`。

## PAL Backend

Desktop 是跨 Linux/macOS 的 target family，不是操作系统 owner。Desktop PAL 的 public component header 是 `libs/pal/providers/desktop/pal_core/include/h2_desktop_platform.h`。它只提供 Memory、Sync、Queue、Log、Time、Task 以及 peripheral、BLE、Wi-Fi、modem、power 等确定性 simulator。Filesystem、Net、entropy、Display、Touch、Audio、Preference、decoder、MQTT、HTTP、TLS 和 WebRTC 来自 OS owner 或独立 reusable provider，由 `app_support` 组合，不在 `pal_core` 中转发。

Netif、SystemEvent、Host Serial、native Bluetooth、Filesystem、Net 和 entropy 分别由 Linux 或 Darwin component 提供。`//libs/pal/providers/desktop/pal_core:pal_core` 不依赖 Linux、Darwin、POSIX 或 vendor package，也不保留 Desktop-named forwarding accessor。`//libs/pal/providers/desktop/app_support:app_support` 按 Bazel target platform 选择 OS provider、创建独立 reusable provider、组装 Runtime，并把同一个 SystemEvent PAL API object 显式传给 Desktop BLE/modem simulator。`app_support` 不直接依赖 private POSIX target。

这些接口是 Desktop target integration surface，不是 app-facing API。跨平台 app 仍然只消费 Runtime 或 `libs` 中定义的公共 contract。

Desktop simulator 和 CoreBluetooth backend 当前不能可靠观察单次 GATT server indication 的 peer confirmation，因此同步 `indicate` operation 在发送前返回 `H2_PAL_ERR_UNSUPPORTED`，不会把普通发送或 notification 描述成已确认。

Desktop backend 使用的 host 能力包括：

- pthread 提供线程和同步能力。
- SDL3 提供 Desktop host 的 display、window 和 input integration；它不进入 T113 或其他嵌入式 target。
- PortAudio 提供音频设备 integration。
- 仓库 gitlink `third_party/wolfssl` 由 `libs/pal/providers/wolfssl` 集成为静态链接的完整 wolfSSL/DTLS 与裁剪 wolfCrypt variants；Desktop 使用 `//libs/pal/providers/wolfssl:wolfssl`，受限固件使用 `//libs/pal/providers/wolfssl:wolfcrypt`。
- SQLite 提供 filesystem-backed preference storage。
- `libs/audio_mixer`、`libs/pal/providers/h2peer` 和 `libs/pal/providers/coremqtt` 提供跨平台 integration。

## OS provider 注入

`h2_desktop_platform_ble(system_event)` 与 `h2_desktop_platform_modem(system_event)` 借用提供 `post` operation 的 SystemEvent API；它们不初始化、销毁或复制 provider，也不要求自己不消费的可选 operation。调用方必须在 simulator 停止前保持该对象有效。相同对象可以重复传入；provider 活跃时不能替换为另一个对象。事件通过注入的 API 发布，不存在隐藏的 Desktop SystemEvent fallback。

macOS Host Serial、CoreBluetooth、Netif 和 SystemEvent 见 [Darwin Components](./darwin)；Linux Host Serial 与既有 Netif/SystemEvent 见 [Linux Components](./linux)。两端共享的 private termios session 见 [POSIX Components](./posix)。

H2Loader native CLI 是 project-owned headless `cc_binary`，不是 Desktop 产品。portable CLI 只消费 Runtime、PAL、Command I/O 与 H2Loader Host Core；target 只组装 Darwin、Linux 或 Windows 已有 provider，并用标准 C stdio 接入 Command I/O。它不拥有 filesystem、socket 或 process 的私有跨平台替代层，也不依赖 Desktop `app_support`、SDL window、layout 或 application bundle。

Desktop unit tests 使用 test-only bounded SystemEvent provider 验证 `NULL`、重复 accessor 与 BLE/modem event delivery，不链接任一 OS component。测试 helper 不进入 `:pal_core` production target。

Desktop BLE simulator 显式实现完整 `h2_pal_ble_vtable_t` shape，但独立 legacy scan-response operation 返回 `H2_PAL_ERR_UNSUPPORTED`，且不修改 advertising-set state。Simulator 继续只保存既有 primary advertising data；不能因为 portable contract 增加可选 operation 就伪造 native scan-response 支持。

## Display 与进程生命周期

一个 `display` 配置只创建一个 SDL window。SDL 只承担 host window、event 和 framebuffer presentation；firmware UI 仍通过 LVGL 渲染到 Display PAL，不在 SDL 层维护另一套 widget 或 XML layout。

Display brightness 通过 framebuffer presentation 的 color modulation 模拟，不能改变 host window opacity。亮度为 `0` 时窗口内显示黑屏，但原生窗口仍保持可见、可聚焦并继续接收模拟按键，使 portable App 能通过正式输入路径恢复亮度。

没有 firmware display 的 app 也必须创建同一个 SDL window，使用户可以通过关闭窗口结束进程。此时 Runtime 的 display capability 使用 canonical unsupported object，Desktop launcher 只在 window 内用 LVGL 显示 app 名称和 `No Display - Running`、`Stopping` 或 `Failed` 状态。当前不支持多个 display 或多个 SDL window。

### Help 与模拟器信息

所有 Desktop window 的标题显示 `H/? Help` 提示。用户按 `H` 或 `?` 时，Desktop PAL 打开统一的原生模态帮助框；shortcut 由 SDL event pump 消费，不转发给 firmware button。打开帮助框前必须释放全部 keyboard-backed button，避免模态窗口期间产生 sticky press。

帮助框包含 `Controls / Help` 和 `Simulator` 两部分。按键说明从当前 keyboard-backed peripheral 配置生成；模拟器信息在打开时读取当前 app title、display 尺寸、power、Wi-Fi、peripheral 类型和状态。Battery 注入值、PWM 当前 duty、button pressed state 等动态数据必须反映打开帮助框时的 snapshot，不能只展示 build-time default。Wi-Fi password、private endpoint、contact phone 或其他 secret 不进入帮助文本。

帮助框是主 SDL window 的 host-native modal dialog，不创建第二个 firmware display、framebuffer 或 portable App route。测试可以通过 `h2_desktop_platform_copy_help_text()` 取得同一份 UTF-8 snapshot，而不实际打开 dialog。

## Video Decoder

`//libs/pal/providers/ffmpeg:ffmpeg` 使用 FFmpeg 的 `libavcodec`、`libavutil` 和 `libswscale` 提供 Audio Decoder 与 Video Decoder PAL。Video provider 接收 packet-fed H.264 Annex-B access unit，并把 frame 规范化到调用方 allocator-backed 的 CPU-readable format；Audio provider 解码 raw AAC-LC，并返回 allocator-backed interleaved S16LE。两个 provider 都不打开路径、不选择 MP4 track，也不依赖 `libavformat`；FFmpeg context、packet、frame、sample format 和 scaler 都属于 provider private state。

`projects/example/targets/cc_binary/mp4-player` 是 MP4 Decoder、Audio PAL 与 Display PAL 的 smoke entry。测试素材使用 Showcase 的 1024×600 Big Buck Bunny 视频流，并把内嵌音轨编码为 Desktop Audio PAL 支持的 16 kHz mono AAC-LC；App 通过 `libs/mp4_decoder` 获取同步 presentation frame，把借用数据复制到三个循环使用的 presentation slot。decoder 和 Audio writer 使用独立 Runtime task，阻塞式 App 调用线程专职呈现 RGB565、轮询 SDL event 并通过 `seek(0)` 循环，避免把 host window 操作迁移到 worker thread。

Launcher 必须持续处理 SDL event。窗口关闭事件是进程退出条件；失去焦点时必须释放所有 keyboard-backed button，避免 sticky press。

触屏式 portable App 通过 Runtime Touch PAL 读取 SDL3 provider 的鼠标坐标与左键状态；需要 LVGL 时，`h2_lvgl_touch_create()` 把同一 Touch PAL 注册为 pointer indev。Desktop launcher 仍在 main thread 轮询 SDL-free host event，App worker 只读取 provider 保存的线程安全 snapshot，不暴露 SDL 类型。

## Strict JSON 配置

每个 entry 的 `layout.json` 由 `tools/bazel/desktop_layout/generate_layout.py` 按 strict schema 验证并生成 C++ config。配置只描述 Desktop integration，不描述 LVGL screen/widget layout：

```json
{
  "display": {
    "title": "Example Desktop App",
    "width": 480,
    "height": 320
  },
  "filesystem": {
    "mounts": [
      {"source": "projects/example/data", "target": "/data"}
    ]
  },
  "peripherals": [
    {"periph_id": 1, "kind": {"button": {"key": "space"}}},
    {"periph_id": 2, "kind": {"nfc_reader": {"simulate": true}}},
    {"periph_id": 3, "kind": {"imu": {"simulate": true}}}
  ]
}
```

`display`、`filesystem` 和 `peripherals` 是必填字段；空集合必须显式写成 `[]`。`simulation` 可省略并使用确定性默认值。Build 在编译 entry 前解析并验证 JSON；duplicate key、未知字段或 peripheral kind、非法 UTF-8/surrogate、非法尺寸、重复 `periph_id`、重复 button key、重叠 mount target、缺失 mount source、路径穿越或任一 symlink component 都必须直接失败。

`simulation` 只描述通过 PAL 暴露的硬件状态，例如 Wi-Fi、Modem、GNSS、温度、电池和按键。联系人、积分、好友、战队、昵称识别、Workflow、对话回复、Pet、更新和配对等业务 fixture 不属于 Desktop layout，出现这些字段必须在 typed ZON 解析阶段失败。业务失败态与边界条件由 portable unit test 自己注入 fixture；默认 Desktop 产品入口必须读取真实服务、持久化状态或明确的 unavailable 状态，不能生成看起来像真实用户数据的演示内容。真实 GizClaw endpoint 使用顶层 `gizclaw` 配置，不放入 `simulation`。

Button 本身不是 window widget，不配置 rect；它把 keyboard key 映射到对应的 PAL `periph_id`。Battery peripheral 提供初始 `voltage_mv`、`percent_x100` 和 flags；PWM switch peripheral 提供初始 `duty_x100`，并允许 simulator snapshot 读取最后设置值。NFC reader 与 IMU 当前只支持 `.simulate = true`。未来 `.simulate = false` 应选择显式 UART adapter 和端口配置，不能静默退回 simulation。Modem 等其他真实外设采用相同原则；H106 confirmed incoming 使用 simulator injection，不把 fake modem 写入 portable App。

## Power 与 Wake Simulation

Desktop power PAL 保存 hold、power state、boot info 和 `boot_count`。`deep_sleep()` 不结束 host process，而是进入 `H2_PAL_POWER_STATE_DEEP_SLEEPING` 并停止 display/audio 等非 wake 行为；`set_hold(0)` 模拟物理掉电并进入 `H2_PAL_POWER_STATE_OFF`，不能再由普通 wake injection 当作 deep sleep 恢复。测试或 launcher injection 模拟新的硬件上电后恢复 `RUNNING`、递增 `boot_count`，并写入新的 boot source/source ID。Portable App 只观察 PAL contract，不 include Desktop header。

Battery、GPIO wake、confirmed incoming 和 power snapshot injection 属于 `h2_desktop_platform.h` 的 launcher/test integration surface。SDL event pump 留在 launcher main thread；阻塞式 portable App entry 在 worker 中运行，窗口关闭通过 app public config 的 cooperative stop callback 请求退出并 join。

## Host Filesystem Mount

`filesystem.mounts` 是多个 read-write mount 的数组。`source` 是仓库根目录相对路径，由 build 解析成绝对 host path；`target` 是 portable firmware absolute path，例如：

```json
{
  "mounts": [
    {"source": "local/h2loader/downloads", "target": "/dl"},
    {"source": "projects/example/apps/audio-system/data", "target": "/data"}
  ]
}
```

Runtime 只看到 `/dl`、`/data` 等 portable path。Host backend 负责 open、read、write、sync、stat、mkdir、clear、remove 和同一 mount 内 rename；不能依赖进程当前工作目录。`..`、未映射路径、重叠 target、symlink escape 和跨 mount rename 必须被拒绝。固件 contract 不区分只读 mount，因此 Desktop mount 统一可读写。

## Peripheral Simulation 与真实设备

Simulation 是 peripheral-kind 的行为，不是通用 `fake` wrapper。Button 由 SDL keyboard event 驱动；NFC reader 在没有注入卡片时返回正常的 no-card/not-found 状态；IMU 默认返回零值。未知 `periph_id` 必须返回 PAL not-found。

外部 NFC reader、modem 等真实 host 设备未来应通过独立 UART adapter 接入。PortAudio 是 audio PAL 的真实设备 backend，不属于 peripheral simulation。`example/audio-system` 要求真实 default input/output device；初始化、设备选择、open 或 start 失败时必须终止启动，不能降级为 synthetic mic 或 sink output。

## Build 与 Package

Desktop 使用 Bazel C/C++ toolchain 作为唯一 build/link driver：

```sh
bazel build --config=macos_arm64 //libs/pal/providers/desktop:all
bazel test --config=macos_arm64 //libs/pal/providers/desktop:all
bazel build --config=linux_x86_64 //libs/pal/providers/desktop:all
bazel test --config=linux_x86_64 //libs/pal/providers/desktop:all
```

Project-owned Desktop entry 的 build、test 和 run 入口是：

```sh
bazel build --config=macos_arm64 //projects/.../cc_binary/...
bazel run --config=macos_arm64 //projects/example/targets/cc_binary/display:example-display
bazel run --config=macos_arm64 //projects/example/targets/cc_binary/audio-system:example-audio-system
bazel run --config=macos_arm64 //projects/e2e/targets/cc_binary/libco:e2e-libco
bazel build --config=macos_arm64 //projects/example/targets/macos_application/tap-reset:tap_reset_app
```

每个 Desktop App 是独立 `cc_binary`，必须通过 exact Bazel label 选择；不存在的 label 直接失败，不从命令行字符串动态生成 target。

可分发的 macOS App bundle 使用 `rules_apple` 的 `macos_application` 直接构建，不先生成 `cc_binary` 再由脚本拼装 `.app`。Tap Reset 的 portable App 与 Desktop adapter 仍是普通 C/C++ Bazel library；`projects/example/targets/macos_application/tap-reset/` 只拥有 native entry、`Info.plist` 和 bundle rule。该目标产出 `tap_reset_app.zip`，其中包含 `GizOSTapReset.app`；需要脚本解析绝对输出路径时使用 `bazel cquery --config=macos_arm64 //projects/example/targets/macos_application/tap-reset:tap_reset_app --output=files`。

`ci-graph` configured analysis 和 Linux native build 在解析 Apple toolchain transition 前通过 `--deleted_packages` 排除该 bundle package。macOS native build 不使用这项 Linux-only 配置，仍会从 project-owned `macos_application` package 构建真实 artifact。真实 artifact 必须由使用 `--config=macos_arm64` 的 macOS build/CI 验证，不能用 Linux graph exclusion 代替 macOS 构建。

H2Loader 的工厂 Batch Loader 是独立 Web 产品，不属于通用 Desktop component，也不由 macOS application bundle 发布。

`macos_arm64` Bazel config 固定 macOS 13.0 minimum deployment target。SDL3、PortAudio、FFmpeg 和 wolfSSL 继续由固定 vendor gitlink 与 Bazel dependency graph 提供；Desktop target 不读取 Homebrew、`/usr/local` 或调用方提供的 dependency prefix。

每个 Desktop package 的 `BUILD.bazel` 只声明自身 source、public header、runtime data 和 exact dependency。主要 ownership 是：

```text
libs/pal/providers/desktop/pal_core/BUILD.bazel
libs/pal/providers/desktop/app_support/BUILD.bazel
libs/pal/providers/sdl3/BUILD.bazel
libs/pal/providers/portaudio/BUILD.bazel
libs/pal/providers/ffmpeg/BUILD.bazel
libs/pal/providers/sqlite/BUILD.bazel
libs/lvgl/BUILD.bazel
third_party/speexdsp.BUILD.bazel
third_party/speexdsp_patch/BUILD.bazel
```

`libs/lvgl` 保留跨平台 LVGL contract、GizOS OSAL integration、upstream core source selection、Display/Touch PAL adapters 和 Desktop/Mobile 编译 variants；它不编译 LVGL upstream SDL2 driver，也不依赖 SDL。SDL3 provider 拥有单一 window、renderer、texture、Display/Touch PAL 和 SDL-free event；Desktop `app_support` 私有拥有 keyboard/wheel 到 LVGL 的 bridge、status surface 和 cooperative close policy。

`third_party/speexdsp.BUILD.bazel` 把完整 SpeexDSP library 暴露为稳定 vendor target；portable type config 由 repository rule 从 `third_party/speexdsp_patch/` materialize 到 external repository。Desktop PortAudio 直接依赖该 target。Embedded target 不编译或链接 SpeexDSP；ESP32-P4 的 AEC 使用 ESP-SR 2.4.7。

Bazel 负责组合 C、C++、third-party library 和 host system library，但不能用来包装 ESP-IDF 或 Armino firmware build。

Desktop PAL 直接依赖上述稳定 vendor label；`rules_apple` 从 Launcher 的 linking context 收集 macOS versioned dylib，不维护第二个 runtime filegroup。Linux foreign build 必须分别声明无 ABI 版本的 linker-name output 和带 ABI 版本的 SONAME runtime output：前者进入 C++ linking context，使 hermetic Zig linker 使用可移植的 `-lfoo`，后者作为独立 Bazel output 供 runfiles 使用。两者都必须在 foreign action 内解引用为普通文件；不能使用 Zig linker 不支持的 GNU `-l:libfoo.so.N`，也不能让声明产物继续指向未被 Bazel 收集的完整版本文件。macOS vendor target 仍把 versioned dylib 直接作为 linking output。Linux 只把 X11、ALSA 等 OS service development boundary 作为明确 host dependency，不能重新引入发行版 curl、SDL2、PortAudio 或 FFmpeg development package。HTTP 由 `libs/pal/providers/corehttp` 静态进入 consumer；Desktop Net PAL 的 TLS 实现由仓库 wolfSSL gitlink 静态进入 binary。Linux x86_64 foreign CMake target 显式使用发行版 dev package 的 `/usr/lib/x86_64-linux-gnu` 查找 X11、ALSA 等 OS service library，并用 `-idirafter /usr/include` 在 hermetic libc headers 之后补充这些 OS service headers；不能依赖 hermetic compiler 自动推断 host multiarch 路径，也不能让 host libc headers 覆盖 toolchain sysroot。两个平台都不能通过 build option、环境变量、Homebrew 或其他 mutable prefix 注入这些 upstream dependency。

Desktop Net PAL 使用固定容量的 asynchronous resolver worker 实现 `resolve_start`/`resolve_poll`/`resolve_close`。portable consumer 的同一个总 deadline 必须覆盖 DNS、connect、TLS 和 HTTP I/O；capacity 用尽时返回 `H2_PAL_ERR_NO_SPACE`，close 不等待 worker，迟到结果由 backend 自行回收。该 worker 只提供 Net PAL contract，不能重新引入 Desktop HTTP backend。

## Validation

Netif 修改至少执行 macOS host 的 PAL/Runtime/Desktop/H106 focused tests，并在
Linux CI 覆盖 rtnetlink adapter。测试应覆盖快照过滤、stale ref、无默认路由、
重复 route message、post failure，以及 notification in-flight 时的 stop/join。

Desktop validation 可以证明 PAL contract、portable logic、parser、network integration 和 simulated hardware behavior。它不能替代真实 board 的 GPIO、flash layout、reset、audio codec、display panel 或 boot flow 验证。

PAL-owned test 放在 `libs/pal/providers/desktop/pal_core/tests/`；reusable provider 的 test 放在对应 `libs/pal/providers/<provider>/tests/`；composition test 放在 `desktop/app_support/tests/`，只有跨多个 package 的 compatibility test 才放在顶层 `libs/pal/providers/desktop/tests/`。HTTP、Network/TLS、MQTT 和 WebRTC 是不同的 validation surface，必须分别提供 test 和 Bazel target。HTTP integration 使用 portable coreHTTP provider；不能恢复 Desktop HTTP backend。

GizClaw E2E 的 portable case、non-fail-fast aggregation、progress 和 cleanup 属于
`projects/e2e/apps/gizclaw/app`。`projects/e2e/targets/cc_test/gizclaw/` 只提供 thin
Desktop harness：从环境变量读取真实 RegistrationToken，装配 Runtime/PAL，加载确定性
PCM，并调用 portable entry。App 不依赖 Desktop、H106、H2Peer 或 Pion；
`libs/gizclaw` 仍只依赖 PAL。Desktop 的 H2Peer target 使用 production WebRTC accessor，
Pion target 则把同一个 App 接到 `libs/pal/providers/pion`，用于相同请求的 transport A/B。
H2Peer target 支持 `connectivity`、`rpc`、`firmware`、`voice`、`concurrency` 和
`all`；Pion target 只支持 `firmware` 与 `voice`，`both` 也只允许这两个 suite。
不支持的组合必须在注册或网络活动前失败，不能把 Pion 描述成完整 H2Peer suite 的
替代实现。

两个 Desktop live target 都是独立的 `manual` `cc_test`：
`gizclaw_h2peer_live_test` 默认在自然入口 `e2e.gizclaw.com:9821` 执行完整 H2Peer suite，
`gizclaw_pion_live_test` 默认在同一入口执行 Pion 的 Firmware 与 Voice suite；manual workflow
可以通过 test environment 选择北京入口或受 backend 支持的单项 suite。真实 token
只通过 `H2_GIZCLAW_E2E_REGISTRATION_TOKEN` 注入；它决定服务端
注册到哪个 RuntimeProfile，通用 GizClaw 正确性测试不接受或断言 H106 profile。Connectivity
覆盖 DNS、TLS/WebSocket、WebRTC 和多轮 ping；RPC
覆盖完整 manifest；Firmware 覆盖 metadata 与 HTTPS 流式下载；Voice 覆盖 PCM 上行、text/Opus 下行、
terminal 和 history audio；Concurrency 固定只使用 owner 的一个 active client/Peer，验证
pinned `v0.3.1` 的 request-owned unary API。它必须报告三个已启动、三个已完成、三个
stream ID 不同的 DataChannel，清理后 open channel 为零，并完成恢复 Ping；不得启动
三个 poll 线程，也不能用三个 client 或三个串行请求伪造并发。Friend/FriendGroup helper Peer
只服务社交资源建模，不计入并发证据。
共享 E2E 环境不注入破坏性 malformed 请求；协议 negative、乱序、截断、错误
metadata、重复 terminal 和反向通道由本地 unit/受控 Pion 测试负责。

两个 backend test 都调用同一个 portable App；需要 A/B 证据时分别显式执行两个 test
label，不使用 Make 聚合入口。Live E2E 通过 exact label 请求 `manual` target，并使用
`--cache_test_results=no`，防止默认自动测试触发外部服务或复用旧结果。

默认 package test 通过 `//libs/pal/providers/desktop/app_support:network_services_test` 验证 wolfSSL、CoreMQTT、H2SCTP 与 H2Peer 的初始化顺序、API wiring 和反向清理。Portable MQTT live flow 属于
`//projects/e2e/apps/pal/app:pal_e2e`；
`//projects/e2e/targets/cc_binary/pal:mqtt_loopback_test` 只装配 Desktop Runtime/provider 和进程内
broker fixture，并通过该 App 完成 connect、subscribe、publish echo 和 disconnect，不依赖外部服务。
访问公共 broker 的 Desktop launcher 也调用同一 portable flow，按需执行：

```sh
bazel run --config=macos_arm64 //projects/e2e/targets/cc_binary/pal:mqtt_public_broker_smoke
```

## 边界

Desktop Component 可以拥有：

- Host PAL backend。
- Simulator 和 simulated peripheral surface。
- Host system library integration。
- Desktop-only package adapter 和 integration tests。

Desktop Component 不应该：

- 让 `projects/<app>` include Desktop header。
- 把 pthread、SDL、PortAudio 或 wolfSSL 类型加入跨平台 Public API。
- 把 host filesystem path 固化为 app contract。
- 向产品入口注入业务 seed、fake provider 或伪造的服务成功结果。
- 在要求真实硬件结果时替代 ESP 或 BK 验证。
- 把 H106、Dinorun 或其他 app 的 Desktop entry 放回 portable app tree。

### 网络状态模拟

Desktop App 的 `layout.json` 可以在 `simulation.wifi_sta.scan_results` 中声明扫描可见的 Wi-Fi，包含 SSID、RSSI、信道、安全类型和加密网络的预期密码；open 网络不得配置密码。`wifi_sta.scan_outcome` 可以配置 `success`、`io_error` 或 `timeout`，`scan_delay_ms` 可以在 60 秒内延迟结果，用于验证失败、PAL timeout、取消和迟到结果；当延迟超过 PAL 调用的 `timeout_ms` 时，Desktop 必须在 deadline 返回 timeout，不能再投递结果。`wifi_sta` 的当前连接状态与扫描列表分开配置。`simulation.modem` 可以声明 modem 是否可用、`mobile_data_enabled` 初始用户期望、运营商、信号和 RAT；不能用瞬时 PPP 连接结果代替初始用户期望。Desktop H106 只在 `cellular_enabled` preference 缺失时用该值初始化 preference，随后通过正式 Modem PAL 恢复状态；用户在页面上的修改继续跨启动保留。Desktop modem 在移动数据关闭时不报告有效 RSSI，H106 在后台恢复或切换完成后也必须立即刷新 Header，不能留下与当前模拟状态不一致的旧信号。Desktop backend 必须复制配置，并通过正式的 Wi-Fi STA 与 Modem PAL 返回和修改状态；App 不能直接读取 JSON 配置。Desktop H106 示例中加密网络的测试密码是 `h106test`。
