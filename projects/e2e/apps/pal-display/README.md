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
bazel test --config=macos_arm64 --lockfile_mode=off //projects/e2e/apps/pal-display/app:interface_coverage_test //libs/pal/providers/sdl3:sdl3_test //projects/e2e/targets/cc_binary/pal-display:no_op_guard_test
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

`//projects/e2e/apps/pal-display:qualification_evidence_test` checks the canonical case ledger, source hashes, managed image identities, two distinct board boots and version-bound human observations. It rejects incomplete qualification or a driver PASS paired with a missing/failed physical observation. Historical board failures remain in the regression receipt.

The real macOS run's LLVM instrumentation covers all 31 App functions and 95.1% of its lines; SDL Display reaches 84.2% of functions, 73.8% of lines and 60.8% of branches. These are E2E-only measurements, separate from focused provider tests. Web/mobile/board evidence records actual calls and output observations without claiming compiler coverage for those platforms or vendor SDKs.

After qualification and confirmation, board launchers perform one bounded 100/50/0/100 inspection cycle, then keep the white-border RGBW quadrants open at stable 100% brightness. `H2_DISPLAY_STABLE` identifies this steady inspection state; repeated result replay does not redraw or dim the panel.
