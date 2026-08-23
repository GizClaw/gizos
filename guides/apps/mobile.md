# Mobile

Mobile 入口归 portable App 的 project owner，不使用独立的平台聚合 project。Browser/WebAssembly 也按相同 ownership 原则归 App owner，但遵守独立的 [Web contract](/apps/web)。

## Structure

```text
projects/example/
├── libs/mobile/
│   ├── tap-reset/                # Tap Reset Mobile adapter 与 contract conversion
│   └── mp4-player/               # Mobile MP4 A/V presentation adapter
└── targets/
    ├── ios_application/tap-reset/ # 生成 iOS .app
    └── android_binary/
        ├── tap-reset/            # 生成 Tap Reset APK
        └── mp4-player/           # 生成大视频 Android APK

libs/pal/providers/
├── ios/pal/                      # UIKit-backed reusable PAL
└── android/pal/                  # JNI/Bitmap-backed reusable PAL
```

`libs/mobile/<app>` 负责该 App 的 Mobile presentation、contract conversion 和 stable adapter entry；`targets/ios_application/<app>` 与 `targets/android_binary/<app>` 分别管理 platform lifecycle、Runtime assembly、package metadata 和最终产物。Portable App 源码仍归 `apps/<app>/app`，`libs/pal/providers/<platform>` 实现跨 project 可复用的 PAL capability。

当前 Tap Reset App 从 `runtime->mem`、`runtime->display` 和 `runtime->time` 取得所有已有公共 capability。Pointer 仍通过 `h2_mobile_app_config_t` 传入，因为公共 Runtime/PAL 尚无 pointer/touch contract；不得把它伪装成已有的 Button input。

Android 只提供一个 MP4 Player smoke APK，用于验证 1024×600 大视频的完整 A/V 播放。它打包 `:large_media`，使用横屏 logical surface，通过 Android MediaCodec 解码 High Profile H.264，通过 NDK MediaCodec 解码 AAC-LC，并由 AAudio 输出 16 kHz mono S16LE PCM。240×240 `startup.mp4` 属于固件侧 `mp4-player-small` launcher，不创建 Android APK。这个 example capability 不包含 microphone、audio focus、route change 或 background playback policy。

## Platform Boundaries

- iOS component owns UIKit surface、frame presentation and touch state；`ios_application` entry owns UIApplication lifecycle、Simulator bundle metadata and the App thread.
- Android component owns reusable JNI view/Bitmap presentation and pointer state；`android_binary` entry owns Activity callbacks、APK packaging and the App thread.

每个 App 都拥有独立 artifact-rule root，因为 package identity、required capability 和 native entry 属于具体 App 产物。App entry 不能复制 Host/PAL backend；增加 App 时应在 `libs/mobile/<app>` 增加 App-specific Mobile adapter，并在 `targets/<artifact-rule>/<app>` 增加实际支持平台的 rule entry。平台 surface、display、input 等可复用实现仍来自 `libs/pal/providers/<platform>/pal`。

## Bazel Build Boundary

Mobile contract、`tap_reset_mobile` App/adapter variant 和两个 platform component 都有 Bazel ownership target。iOS PAL、launcher 和 bundle 已接入真实 `rules_apple` dependency graph；Android Activity、JNI/PAL、Runtime 和 APK 已接入真实 `rules_android` 与 `rules_android_ndk` dependency graph。两个 `tap_reset_app` target 都必须直接编译 portable App、Runtime、PAL 和 Mobile LVGL variant，并生成对应 Simulator `.ipa` 或 arm64 APK。

MP4 Player adapter 与 Android launcher 同样由 Bazel ownership target 管理；最终 `mp4_player_app` APK 必须直接包含 portable MP4 Player、MP4 Decoder、Runtime、Android PAL 和且仅有一份 `:large_media` asset。

Linux host build 通过 target compatibility 跳过 iOS、Android package；实际 iOS lifecycle test 和 artifact build 由装有 Xcode 的 macOS CI 使用 `ios_sim_arm64` config 完成，Android artifact build 由装有 SDK/NDK 的 Linux CI 使用 `android_arm64` config 完成。

本地已有对应 SDK/toolchain 时，直接通过 Bazel 运行与 CI 相同的 package acceptance：

```sh
bazel test --config=ios_sim_arm64 //projects/example/targets/ios_application/tap-reset:package_validation_test
bazel test --config=android_arm64 //projects/example/targets/android_binary/tap-reset:package_validation_test //projects/example/targets/android_binary/mp4-player:package_validation_test
```

iOS validator 检查 bundle identity、arm64 Simulator executable 与 bundle content；Android validator 检查 debug signature、package/activity identity、arm64-v8a native library，并对 MP4 Player 检查唯一 large-media asset。每个 execution class 直接分析和构建自己的完整 compatible graph；不能把其他平台跳过 package 误报为移动平台验证成功。

Mobile 直接 Display PAL consumer 使用 `//libs/lvgl:lvgl_mobile` compile-time variant。它复用 `//libs/lvgl:single_thread_config` 的 `LV_OS_NONE`、libc allocator 和 RGB565 配置；第三方 overlay 只提供 upstream source/header group，配置和可编译 variant 都属于 reusable LVGL library，不能按 App 创建 `lvgl_<app>` target。

## Production Gap

两个 target 当前都只是 smoke/example backend，不表示 GizOS 已经完成 iOS 或 Android 平台支持：

| Capability | iOS | Android |
| --- | --- | --- |
| Memory、Time、Queue、Display | Example implementation | Example implementation |
| Pointer/touch | Launcher callback bridge | Launcher callback bridge |
| Task、Sync | Unsupported | Example Task；Sync unsupported |
| Audio | Unsupported | Example 16 kHz mono playback 与 raw AAC-LC decode；mic unsupported |
| Persistence、network、BLE 等其余 PAL | Unsupported | Unsupported |
| Production lifecycle、signing、physical acceptance | Not completed | Not completed |

生产 App 必须根据 required capability 补齐对应 platform component，并完成 permissions、foreground/background lifecycle、release signing 和 physical-device acceptance；不能把这个 example 的成功运行当作平台完成证据。
