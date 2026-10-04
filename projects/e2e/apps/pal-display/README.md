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

The six-platform physical/UI qualification snapshot is retained at `b6d6daa6`. Subsequent review fixes have separate current-source and validation records: only successfully started BK PWM is running, partial init/start failures release it, failed release retains cleanup ownership, and retry must recover before changing duty. The production helper's host regression injects native init/start/update/deinit errors. SDL validates a C caller's unknown enum representation before any C++ enum load; enum-sanitized real-renderer qualification and silent-no-op rejection both pass. UTF-8 receipt decoding is explicit on Windows. Both actual mobile Audio SDK consumers were rerun 24/24 after the shared Display provider change. Current BK native build identity is recorded separately and is not relabeled as an R13 physical execution or a new human observation.

## Shared mobile runner evidence

The shared mobile runner's optional-capability and direct Bazel declarations is validated with fresh iOS/Android Display observations executed from `e5bd1bef794e4680d42af92583ffa2ca59936dd4`. Both runs pass the same 24-case Display contract; the default empty optional list preserves the original generated declaration hash. `mobile_runner_refactor.json` binds those actual timestamps, device identities, App/SDK hashes, executed Python/BUILD/macro hashes and generated declaration hashes. These current acceptance observations replace the earlier `a0895cbf` and `99867cb3` mobile observations, which remain in Git history. Its current host/audit source receipts and explicit removed-shell receipts replace only those host files in the source check. A fixed audit allowlist separately binds the Make entrypoints and shared E2E/PAL guides changed to describe Net/TLS; no native provider or App source can enter that allowlist. Every native provider, portable App, registry and board source receipt is still checked unchanged. The original `qualification.json` and hardware receipts retain their historical identities; no new physical run is claimed by this host-runner validation.

Display 的包名、报告、registry、期望字段、SDK 符号和 PNG 捕获在 `pal-display-mobile/BUILD.bazel` 的 `mobile_e2e_suite` 中声明；Python hook 仅解析原生行式报告。独立移动端证据同时绑定该 BUILD、公共宏和实际执行声明的 SHA-256，旧硬件资格记录保持不变。

The shared E2E catalog is checked by Display ownership: its complete `## PAL Display` section and unique row inside the `## Apps` launchers table must be byte-identical to commit `06f9c0cfa633646984d72f210ba18f889bbec528`. Moving that row outside the table or into a fenced code sample fails admission. `shared_catalog_baseline.txt` archives that commit's complete guide, authenticated by the original whole-file hash in `gizclaw_harness_provenance.json`; no keyword-only or current-file hash replacement can admit a changed Display contract. Other Apps may update their sections independently. `shared_catalog_provenance.json` separately binds only this host validator, its Bazel inputs and this README to their previous hashes. All other source checks and every historical qualification, executed mobile, artifact and hardware receipt remain unchanged. This audit maintenance claims no new platform or physical execution.

Reproduce the host audit inputs from the reachable immutable Git objects with `python3 projects/e2e/apps/pal-display/record_shared_catalog_provenance.py --apply`; run the same command without `--apply` to verify the committed baseline, exact Pref extension and provenance. Then run `//projects/e2e/apps/pal-display:qualification_evidence_test` and `//projects/e2e/apps/pal-display:mobile_runner_provenance_test`.

The shared PAL guide keeps its original whole-file source hash. The only admitted extension is the exact two BK Pref paragraphs from immutable source `93c9578e54f1cd45d8d9bd118372f807e16076f4`, archived in `shared_pal_pref_addition.json` and inserted before the historical ESP LittleFS paragraph at the recorded byte offset. Removing exactly those authenticated bytes must reconstruct the original complete guide hash; the unchanged guide also passes. Changed Display text, unknown additions, a different insertion position or any other PAL document edit fails admission. The reproducer requires both source objects and regenerates only the three new host-audit inputs. The new audit sidecar records this explicit extension independently of all unchanged historical qualification receipts.
