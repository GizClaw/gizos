# PAL Audio Decoder E2E

独立 portable C App 验证 `h2_pal_audio_decoder_api_t` 的全部 8 个操作。它只借用 Decoder、Memory、Time、Sync PAL；具体 AAC provider、Runtime assembly、系统 SDK、模拟器和串口属于 launcher。Audio Decoder 是独立能力，这份资格不表示录音、扬声器或 Audio track 通过。

29 个 mandatory case 覆盖 open/configure、非法参数和 ASC、非阻塞和有限等待、RAW AAC-LC 单/双声道解码、PCM format/length、时间戳、原始频谱和声道分离、EOS 完整 PCM 总量与严格递增时间戳、借用 ASC/packet 生命周期、持有 frame 时的 acquire/reset/close、foreign frame、重置和切换格式、EOS、实际 PCM allocator 使用、12 个位置的 allocation failure 和重复 session。所有 case 独立执行并报告完整结果；缺失能力记为 BLOCKED，失败或残留资源不授予资格。发生 release/close 失败时保留 allocator context，launcher 必须保持所借用依赖有效，不能释放其 Runtime。

原始 fixture 是生成的正弦波：44100 Hz mono 997 Hz、48000 Hz stereo 997/1999 Hz，各 13 个 RAW AAC-LC packet 和对应 ASC。测试允许 encoder priming 和各 codec 的舍入差异，但必须验证实际非零 PCM、正确格式和频谱。fixture 生成命令与摘要见 [fixtures](app/fixtures/README.md)。不使用第三方音乐，也不以 decoder 成功返回替代内容验证。

```sh
bazel test --lockfile_mode=off \
  //projects/e2e/apps/pal-audio-decoder/app:interface_coverage_test \
  //projects/e2e/apps/pal-audio-decoder/app:reject_missing_test \
  //projects/e2e/targets/cc_binary/pal-audio-decoder:desktop_test

# 明确选择具备 AAC WebCodecs 的浏览器，使用独立临时 profile。
H2_WEB_TEST_BROWSER=/path/to/AAC-capable/browser \
  make bazel-test-wasm_pal_audio_decoder_browser_test
H2_IOS_SIMULATOR_UDID=<booted-uuid> make bazel-test-ios_pal_audio_decoder_simulator_test
ANDROID_HOME=<sdk> ANDROID_NDK_HOME=<ndk> H2_ANDROID_SERIAL=<emulator-serial> \
  make bazel-test-android_pal_audio_decoder_simulator_test
```

macOS 使用 FFmpeg；Web 使用真实 WebCodecs，C 运行在 pthread Worker，UI output callback 仅使用有界私有 staging buffer，调用方 PCM allocator 在 Worker acquire 时执行。未带 AAC codec 的开源 Chromium 返回 BLOCKED，不计通过；浏览器 manual 入口使用宿主 Python，保留 HOME 以正常启动已安装的 Chrome，并始终创建独立临时 profile；保留实际产品/版本、二进制和 Web archive 摘要，资格仅适用于该 AAC-capable browser runtime。iOS 使用原生 AudioConverter，ASC 转换为 MPEG-4 ES descriptor cookie；Android 使用 AMediaCodec。移动端实际安装 IPA/APK，验证 Swift Package/XCFramework 和 AAR 的 provider 导出及实际打包二进制；模拟器结果不代表物理手机。

设备使用现有 ESP AAC codec 与 BK Helix decoder。DevKit 不需要音频输出硬件，因为这里验证解码所得 PCM。先 source 固定 firmware-devenv 环境，BK7258_PATH 指向对应干净的 SDK；磁盘编译缓存保持开启。

```sh
bazel build --config=esp32s3 --lockfile_mode=off \
  --//tools/bazel:firmware_version=pal-adec-esp-<unique-version> \
  //projects/e2e/targets/h2loader_tar_zlib/pal-audio-decoder/devkit:package
bazel build --config=bk7258 --lockfile_mode=off \
  --//tools/bazel:firmware_version=pal-adec-bk-<unique-version> \
  //projects/e2e/targets/h2loader_tar_zlib/pal-audio-decoder/bk7258_v3_202405:package
```

设备验收须核对 UID、原 P1 Loader 和 coredump，send 后核对 Stage 的版本及 package/image checksum，再执行 managed upgrade。只有 29 项全通过且资源全部释放才 confirm App，成功 marker 为 `H2_ADEC_READY rc=0 confirm=0`；失败保留明确 marker，不能确认。随后独立正常 App reboot 再跑一轮，最终核对 P1 保留、Stage 空、last_result=0 和真实 coredump 基线。重连后的 immutable ledger replay 不算独立运行。报告中的 retained=0 表示 Decoder session/PCM 已完整释放；设备 Runtime 持续服务 H2Loader 命令，不将其描述为 Runtime teardown。

当前执行证据正在收集：macOS、AAC-capable Chrome Worker、iOS Simulator、Android Emulator 各 29/29；DevKit 修复版托管安装与独立正常 reboot 各 29/29，P1 保留、Stage 空、coredump blank；BK7258 尚未执行本 App。不能把构建成功或部分平台结果称为六端通过。

macOS 上真实 FFmpeg 与原生 AudioConverter 的 LLVM covmap 验证：portable App 383/419 行（91.4%）、45/47 函数（95.7%）、218/360 分支（60.6%）；iOS AudioConverter provider 245/260 行（94.2%）、15/15 函数；FFmpeg Audio Decoder provider 239/281 行（85.1%）、15/16 函数。Android portable contract helper 为 107/113 行，11/11 函数。它们是这些具体 host 执行路径的编译器覆盖率，不能代替 Web、MediaCodec 或板端代码覆盖率。所有 LLVM DA 记录均核对在源码行界限内。
