# Mobile Platform Components

`libs/pal/providers/ios/pal_core`、`libs/pal/providers/android/pal_core` 和 `libs/pal/providers/web/pal_core` 保存仓库级可复用的平台 PAL backend。它们属于平台能力，不属于某个 Mobile App，也不属于产生 `.app`、APK 或 HTML/JS/WASM 的 artifact entry。

## Ownership

每个平台 component 提供自身已实现的 PAL accessor，并把 native surface 或 pointer state 暴露给平台入口。Android 还提供基于 pthread 的 Task PAL、AAudio playback PAL，以及 NDK MediaCodec H.264 Video Decoder 和 AAC-LC Audio Decoder PAL，用于 MP4 Player 的 bounded A/V lifecycle：

- iOS component 管理 UIKit view、RGBA frame 和 touch snapshot。
- Android component 管理 JNI view reference、RGBA frame、Bitmap copy、pointer snapshot、16 kHz mono S16LE playback track、raw AAC-LC 到 allocator-backed PCM 的转换，以及 H.264 到 allocator-backed RGB565 frame 的转换。
- Web component 管理 libco cooperative executor、Canvas frame presentation、Emscripten pointer handler、origin-scoped `localStorage` Pref、Web Serial Promise bridge 和 browser lifecycle cleanup。

Component 的 display size 必须通过 public config 传入，不能依赖具体 project 的固定尺寸、App config 或 adapter 类型。Component 只能依赖 PAL contract 和对应平台 SDK；不能选择 App、组装完整 Runtime、拥有 package identity 或决定进程生命周期。

这些目录只为已经实现的 capability 负责，不因目录存在而成为完整平台 backend。iOS 实现 example 所需的 Memory、Time、Queue、Display 与 native pointer bridge；Web 实现 Memory、Log、Time、Timer、Task、Queue、Sync、Pref、Display、Touch 与 Host Serial；Android 额外实现 Task、固定 16 kHz mono playback、raw AAC-LC decode 和 CPU-readable H.264 decode。Web Pref 仅承诺同一 origin 下的 typed key/value 持久化，不提供跨 origin、跨浏览器同步或物理串口身份。未列出的 network、BLE 等 PAL 保持 canonical unsupported。任何新 App 接入前都必须先枚举 required capability，不能从目录存在或 smoke App 成功启动推导出平台已完成。

Darwin host provider 与 iOS mobile provider 是独立 ownership root。macOS Netif、SystemEvent、Host Serial 与 CoreBluetooth 归 `libs/pal/providers/darwin`；iOS UI、mobile lifecycle 与 iOS PAL 归 `libs/pal/providers/ios`。两端不能通过互相依赖来共享 App、UIKit、CoreBluetooth delegate 或 Runtime assembly。

iOS CoreBluetooth provider 不提供独立 legacy scan-response 配置；`h2_pal_ble_adv_set_set_scan_response_data()` 由完整 vtable entry 显式返回 `H2_PAL_ERR_UNSUPPORTED`，不能依赖零初始化 slot，也不能把 scan-response 内容合并进 primary advertising data。

Android Audio 与 AAC Decoder provider 共用的参数校验、PCM layout/copy、allocator failure 和 platform error mapping 必须保持 platform-independent，并由 `//libs/pal/providers/android/pal_core:audio_contract_test` 在 host 覆盖；真实 MediaCodec、AAudio、AudioFlinger 和 Activity lifecycle 继续由 Android Emulator acceptance 覆盖。成功播放不能替代 invalid codec/bitstream、malformed PCM、allocation failure 或 platform error 的 PAL result 验证。AAudio 已接受部分 frame 后，track 必须持有并在下一次 write 或 drain 前继续提交剩余 PCM；不能返回会让调用方重放整个 frame 的 retryable result。

Android H.264 Video Decoder 的 stream validation、planar YUV layout bounds 和 RGB565 conversion 必须保持 platform-independent，并由 `//libs/pal/providers/android/pal_core:video_contract_test` 在 host 覆盖。真实 codec selection、output format/stride、High Profile decode、loop reset 和画面显示继续由 Android Emulator acceptance 覆盖；provider 只能读取当前 output buffer 声明的有效字节，不能用底层 buffer capacity 替代 frame size。

Web Task/Queue/Sync 由 libco 的 Emscripten Fiber/Asyncify backend 在单个浏览器线程上协作调度。同步 PAL 调用只能等待会由后续 bounded platform pump 产生的事件；platform 必须为最早 task/Timer deadline 和外部完成自动安排 pump，不能依赖无关的 UI event 或 launcher polling 才能推进。实现不提供抢占、CPU 并行或跨 Worker 同步，也不要求 `SharedArrayBuffer`。Promise callback 只提交 generation-tagged completion 并请求后续 turn；pump 必须避开活动 Asyncify export，callback 不能直接恢复 task 或访问已经销毁、复用的 WASM state。

## Artifact Entry Boundary

`projects/<owner>/targets/{ios_application,android_binary,pkg_tar}/<app>` 负责：

1. 创建对应 platform component。
2. 选择 component 提供的 PAL capability，并为暂不支持的 capability 装配 canonical unsupported API。
3. 初始化和销毁 `h2_runtime_t`。
4. 把平台 pointer snapshot 投影成对应 Mobile 或 Web adapter contract。
5. 管理线程、Activity、UIApplication 或 browser entry，并生成对应平台产物。

新增其他 project 的 iOS、Android 或 Web entry 时应复用这些 component，而不是复制 PAL backend。Web entry 不能复用 Mobile App contract。需要生产能力时，在 component 中增加可复用 capability 实现，再由 artifact entry 按 App 的 required capability 组装 Runtime。

iOS launcher 在销毁 platform 前必须先停止并 join App thread、释放 Runtime。Platform destroy 在 UIKit main queue 上使 surface 与 host 解除关联并移出 view hierarchy，再释放 framebuffer、mutex 和 host memory；已经排队的 redraw 或后续 touch callback 看到 detached surface 后直接返回，不能访问已经释放的 host。

## Build Boundary

iOS component 由真实 `objc_library` 编译，并作为 `rules_apple` `ios_application` 的 dependency；iOS launcher 不保留第二套 CMake source list。Compatibility 同时要求 iOS OS constraint 与 `ios_sim_arm64` config identity，避免 Apple transition 在其他完整 graph 中误选 iOS bundle。Component lifecycle 使用 `ios_unit_test` 在 Simulator 上验证。Linux CI 通过 platform compatibility 跳过 iOS target，macOS CI 使用 `ios_sim_arm64` config 负责 lifecycle test 和完整 Simulator artifact build。

Android component 由 NDK configuration 下的真实 `cc_library` 编译，通过 `android_library` native dependency 进入 `rules_android` `android_binary`；Android launcher 不保留第二套 CMake 或手工 APK script。`android_arm64` build config 把顶层 target platform 固定为 `arm64-v8a`，使完整 `//...` build 通过 compatibility 跳过普通 Linux target；`rules_android` 继续把 native dependency transition 到相同 Android platform。Package validator 单独把顶层 platform 覆盖为 Linux x86_64，从 host execution platform 检查生成的 APK，但仍保留 `android_arm64` config identity 和 `--android_platforms=arm64-v8a`。`rules_android_ndk` 从 `ANDROID_NDK_HOME` 发现并注册 C/C++ toolchain，`rules_android` 从 `ANDROID_HOME` 发现 SDK、编译 Java、合并 native shared object 并 debug-sign APK。普通 Linux config 通过 platform compatibility 跳过 Android target，独立 Android CI job 使用 `android_arm64` config 负责完整 APK build。

Platform artifact acceptance 直接走 Bazel：iOS 使用 `bazel test --config=ios_sim_arm64 //projects/example/targets/ios_application/tap-reset:package_validation_test`；Android 使用 `bazel test --config=android_arm64 //projects/example/targets/android_binary/tap-reset:package_validation_test //projects/example/targets/android_binary/mp4-player:package_validation_test`；Web 使用 `make test-web`。这些 validation targets 校验最终 IPA/APK/Web archive 的 identity、architecture、签名或归档入口；Web validation 还通过 Emscripten/Node 执行 PAL、libco、portable E2E registry 与 fake Web Serial tests，不以 graph-only query 代替产物验证。
普通 host `cc_test` 继续由 Linux/macOS config 执行，不能编成移动平台二进制后直接交给 runner OS 执行。

Web artifact 由 `@emsdk` 的 `wasm_cc_binary` 使用真实 Emscripten toolchain 编译，再由 `@rules_pkg` 的 `pkg_tar` 打包。不得用 `genrule` 套壳来冒充 toolchain 或 packaging rule。
