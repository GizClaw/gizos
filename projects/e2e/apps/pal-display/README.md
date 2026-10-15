# PAL Display E2E

Portable App `//projects/e2e/apps/pal-display/app:pal_display_e2e` receives a borrowed Runtime and a launcher-owned observation callback. It calls every Display vtable operation: `open`, `get_info`, `draw_bitmap`, `present`, `set_brightness_percent`, `close`. The file-scope registry contains 24 mandatory cases. A missing interface, failed observation, failed cleanup or unrun case fails qualification; managed launchers confirm only after complete success.

## Cases and capability profiles

| Cases | Observable behavior |
| --- | --- |
| Interface/null/closed/open/info | Every slot exists; invalid arguments and closed state fail; open is idempotent; dimensions and native RGB565 are valid |
| Full borrowed frame, partial draw, padded unaligned source | The real output matches independently constructed RGB primary quadrants and white border, including the small patch; borrowed input is overwritten/freed before present |
| RGB888/RGB444, unknown format | Each fixed profile either converts the supported format correctly or returns UNSUPPORTED without modifying output; invalid enum is rejected |
| Empty/short/outside/partial/overflow bounds | Valid cropping/rejection follows the fixed provider profile; invalid row/span arithmetic fails without changing output |
| Repeated present, brightness 0/50/100/invalid | Output remains stable across presents; UI pixels reflect dimming, boards execute real brightness commands/PWM updates; invalid >100 input preserves brightness |
| Close, duplicate close, reopen | One close after duplicate open closes the surface, closed operations fail, two new lifetimes can render the complete independent pattern |

Native RGB565 is mandatory in all six profiles. SDL, AMOLED and BK support RGB888/RGB444 and rectangle clipping. Web, iOS and Android currently support native RGB565 only and reject partially outside rectangles. Unsupported optional formats are tested as explicit contract results, not reported as format-conversion support. The public contract does not promise universal clipping.

## Output observations

- Desktop uses an optional **actual renderer** capture (`SDL_RenderReadPixels`) after texture conversion and brightness modulation. The older composition/framebuffer capture remains separate; recording consumers keep their original behavior.
- Chromium runs C main on a real pthread Worker. The existing single main-thread proxy waits for a test fixture to capture the composited page. The Python verifier decodes real `Page.captureScreenshot` PNGs and checks every pixel, including the canvas CSS brightness filter. A missing fixture times out and fails.
- iOS consumes the built Swift Package/XCFramework and captures the actual UIKit View after layout/display. The logical point size and screen scale yield one captured pixel per PAL pixel; captured colors are normalized to sRGB before comparison.
- Android consumes the actual AAR (the APK copy must be byte-identical), renders its provider Bitmap in an attached Activity View, and captures that View's completed Canvas output on the UI thread. Retirement drains UI use before provider destruction.
- AMOLED taps **completed SPI DMA** chunks after the real panel transaction drain and compares those bytes after panel byte-order decoding. This is transfer evidence, not optical pixel readback.
- BK captures the **active LCD DMA source** and checks that the hardware refresh counter advances. This observes the controller's actual scanout source, not the PAL software shadow. It still does not prove optical panel pixels or measured brightness.

The board brightness paths send actual SH8601 brightness commands or use GPIO7/PWM1 for the BK backlight. BK's SDK default PWM1 pin is GPIO19 (LCD_R7); the board GPIO table selects GPIO7 to preserve red pixel data. All board GPIO profiles use the same safe PWM table. BK AP PSRAM system heap is 640KB; its separate media/display slab holds the large expected/input/capture working sets through a launcher-owned real Memory API. App config borrows this working Memory API; Core Runtime keeps its original allocator and ABI. The visual demo streams rows to avoid large Core heap allocations. The original Loader/P1 and coredump remain untouched.

The BK launcher selects a Display-specific H2Loader GPIO profile. It keeps the known working board's 29 RGB clock/control/data mappings and preserves GPIO0/1 for Loader UART1. The SDK does not initialize RGB pins itself, and its runtime GPIO mapper rejects pins omitted from the selected profile; a UART-only profile can produce valid DMA/refresh evidence with no image on the panel. The board provider therefore validates those RGB mappings on every open and restores only incorrect mappings. H050IWV has no SPI initialization callback, so its optional SPI control pins are disabled to preserve UART1.

## Validation

```sh
bazel test --config=macos_arm64 --lockfile_mode=off //libs/pal/providers/sdl3:sdl3_test //projects/e2e/targets/cc_binary/pal-display:no_op_guard_test
bazel run --config=macos_arm64 --lockfile_mode=off //projects/e2e/targets/cc_binary/pal-display:pal_display_e2e
bazel test --config=macos_arm64 --lockfile_mode=off //projects/e2e/targets/pkg_tar/pal-display:browser_test
bazel test --config=ios_sim_arm64 --lockfile_mode=off //projects/e2e/targets/ios_application/pal-display:ios_pal_display_simulator_test
bazel test --config=android_arm64 --lockfile_mode=off //projects/e2e/targets/android_binary/pal-display:android_pal_display_simulator_test
bazel build --config=esp32s3 --lockfile_mode=off //projects/e2e/targets/h2loader_tar_zlib/pal-display/amoled:package
bazel build --config=bk7258 --lockfile_mode=off //projects/e2e/targets/h2loader_tar_zlib/pal-display/bk7258_v3_202405:package
```

The desktop binary opens a real window. Linux CI's focused desktop test uses SDL's software/dummy surface and is integration coverage only; it is not the real macOS qualification. Simulator runners require explicit `H2_IOS_SIMULATOR_UDID`/`H2_ANDROID_SERIAL` plus SDK tool paths. Native builds require the existing versioned SDK/toolchain locators. Keep the configured Bazel disk cache enabled.

Install board Apps with H2Loader `send --file`, verify staged identities, then `reboot upgrade --monitor`. Require a new `H2_DISPLAY_BOOT`, the **run** ledger and `H2_DISPLAY_READY rc=0 confirm=0`; repeated **replay** ledgers are not additional executions. Repeat with an independent `reboot app --monitor`. Final status must have original P1, Stage empty, running/next partition App and unchanged coredump. A failed qualification does not confirm its managed image.

Physical panel qualification requires a camera/readback fixture or an explicit human observation of the color pattern and stable output. The user confirmed both R13 boards correctly and stably show the white-border RGBW image. This confirmation does not establish observation of every brightness step: 0/50/100 and invalid input are covered by the real controller-command/PWM path and mandatory case ledger, while optical brightness measurement remains `NOT_MEASURED`. Driver-only PASS cannot promote an unverified physical image claim. Compact receipts record these boundaries and source/package/report hashes; raw logs, dumps and screenshots stay local.

当前版本运行相同的 24-case C Display 合约，并分别验证真实 provider、屏幕观察、清理和产物身份。历史 qualification 和 hardware receipts 保留原始 revision、源码和产物摘要，不把当前源码与旧快照逐字匹配作为行为验收。

The real macOS run's LLVM instrumentation covers all 31 App functions and 95.1% of its lines; SDL Display reaches 84.2% of functions, 73.8% of lines and 60.8% of branches. These are E2E-only measurements, separate from focused provider tests. Web/mobile/board evidence records actual calls and output observations without claiming compiler coverage for those platforms or vendor SDKs.

After qualification and confirmation, board launchers perform one bounded 100/50/0/100 inspection cycle, then keep the white-border RGBW quadrants open at stable 100% brightness. `H2_DISPLAY_STABLE` identifies this steady inspection state; repeated result replay does not redraw or dim the panel.

The six-platform physical/UI qualification snapshot is retained at `b6d6daa6`. Subsequent review fixes have separate current-source and validation records: only successfully started BK PWM is running, partial init/start failures release it, failed release retains cleanup ownership, and retry must recover before changing duty. The production helper's host regression injects native init/start/update/deinit errors. SDL validates a C caller's unknown enum representation before any C++ enum load; enum-sanitized real-renderer qualification and silent-no-op rejection both pass. UTF-8 receipt decoding is explicit on Windows. Both actual mobile Audio SDK consumers were rerun 24/24 after the shared Display provider change. Current BK native build identity is recorded separately and is not relabeled as an R13 physical execution or a new human observation.

## Shared mobile runner evidence

The shared mobile runner executes the native Display contract and records actual device identities, timestamps, App/SDK identities and observations. Historical runner/provenance records remain archival evidence for their recorded revisions; they do not freeze the current runner or documentation source.

Display 的包名、报告、registry、期望字段、SDK 符号和 PNG 捕获在 `pal-display-mobile/BUILD.bazel` 的 `mobile_e2e_suite` 中声明；Python hook 仅解析原生行式报告。独立移动端证据同时绑定该 BUILD、公共宏和实际执行声明的 SHA-256，旧硬件资格记录保持不变。

The shared E2E catalog and platform guides describe current ownership and commands. Historical baselines and provenance files remain archival records; no source-hash allowlist or exact paragraph/Makefile byte comparison gates current changes.

Use the current native/browser/device test entrypoints above to assess current code. Historical JSON records are not rewritten to claim new physical execution.

The shared PAL guide keeps its original whole-file source hash. Outside the independently owned Modem region, the only admitted extension is the exact two BK Pref paragraphs from immutable source `93c9578e54f1cd45d8d9bd118372f807e16076f4`, archived in `shared_pal_pref_addition.json` and inserted before the historical ESP LittleFS paragraph at the recorded byte offset. The validator independently pins their original digest `e34e59d847676c1d0bca583f8c824a124b1a8cb0dd684b097dcdc25aff429932`; changing the guide, addition fixture and sidecar together cannot replace those bytes. After projecting the Modem-owned region onto its historical bytes, removing exactly those authenticated Pref bytes must reconstruct the original complete guide hash; the unchanged guide also passes. Changed Display text, unknown additions, a different insertion position or any other PAL document edit outside that Modem region fails admission. The reproducer requires the immutable source objects, verifies their independently pinned digests and regenerates only the five host-audit inputs. The new audit sidecar records this explicit extension independently of all unchanged historical qualification receipts.

The Modem-only region starts at the unique `### Modem 通话扬声器音量` heading and ends before the unique `### Touch` heading; its only permitted headings are the original volume section and the emergency/OTA section. The validator detects tab-separated, indented and empty ATX headings and rejects them unless they match those exact headings; Setext headings/underlines are outside this closed profile. `shared_pal_modem_baseline.json` archives the original region from the same immutable baseline commit, with its digest pinned independently in the validator. Host validation projects only that unrelated region onto the authenticated original bytes before applying the existing exact Pref-extension and whole-guide hash checks. Missing/duplicate/moved boundaries, a non-Modem heading, a forged baseline, changed Display/general PAL text and altered Pref bytes still fail. The reproducer regenerates this baseline and the separate host audit sidecar without updating any qualification, mobile execution, artifact or hardware receipt.

The Web backlight setter additionally exposes its existing percentage to the host in `Module.h2WebOutputs`. Host audit admits exactly the two snapshot-assignment lines immediately after the existing CSS brightness write. Removing those lines must reconstruct the complete historical provider SHA-256. Display rendering, brightness application and every other operation remain byte-identical; modified/relocated assignments or changed rendering code still fail. This browser observation is not a new physical qualification. The existing Web PAL output test validates the host snapshot independently.

## BK RGB buffer requalification

The RGB provider now retains its canonical shadow and two immutable scanout buffers. Each slot accumulates changes until the SDK returns its write ownership; only those pixels are copied before its next submission. The three-frame working set stays unchanged. Sparse diagnostic samples remain separate from the complete active-DMA comparisons.

`bk_rgb_buffer_requalification.json` binds only the changed BK framebuffer sources to two fresh 24/24 executions of commit `75d83ca4a9c9297cc7018ce3d2d0cdb0be87fe84`, the exact package/image identity, unchanged Loader/P1, empty stage and the user's stable RGBW pattern confirmation. The independent reboot excludes result replay received before the new BOOT. The original six-platform qualification and mobile receipts are unchanged; the new record cannot exempt another provider or a failed/incomplete case. Coredump invariance was not sampled for this focused framebuffer follow-up, and physical luminance was not measured. H106 page-transition performance is a separate consumer result.

`bk_display_guide_format_provenance.json` retains the original guide snapshot and its recorded digest as historical context. Current guide wording and formatting are not constrained by that snapshot; framebuffer, package and hardware records keep their original identities and describe only the recorded physical executions.

## AMOLED DMA ceiling

The AMOLED driver accepts an opt-in DMA row ceiling of 8, 16, 32 or 64; zero keeps the existing 64-row default. The production driver calls the same ceiling function exercised by `//boards/amoled/esp32s3:display_config_host_test`. The iperf launcher selects 8 rows and FT3168 adds read-error diagnostics. Current-source behavior is verified by executable C tests and actual native builds. Historical DMA/device receipts keep their original source/artifact identities; the new row choice has no current physical or finger/optical acceptance.
