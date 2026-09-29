# PAL Audio E2E

`pal-audio` is an independent portable App for the Audio PAL in `libs/pal/include/h2/pal/hal/h2_pal_audio.h`. Its 24 mandatory cases cover all 11 Audio provider operations and all five track operations. The suite exercises argument and format boundaries, real PCM capture and playback, volume and microphone gain changes before and during capture, track drain/close, repeated resource opening, and at least 30 seconds of simultaneous record/playback. Every launcher passes a borrowed **real platform Audio PAL** directly; the Board Runtime exists for time and H2Loader commands, not as a substitute for the provider under test. A missing capability is `BLOCKED` and prevents the qualified verdict. The focused `cleanup_once_test` guards against a second, unreported gain/volume restore and injects transient and permanent close failures. Terminal cleanup retains incomplete-close handles for at most three attempts, defers speaker stop while a track remains, and fails qualification if cleanup cannot finish; the launcher then tears down its provider.

The App reports the count, peak and sum of squared magnitudes for returned microphone PCM, plus generated output PCM peak, frame count and measured soak duration. Desktop additionally witnesses samples delivered through PortAudio's real-device callbacks. Browser testing uses Chromium's deterministic fake microphone source through the production `getUserMedia` and AudioWorklet PAL; it does not claim a physical browser microphone. The mobile runs use simulator audio devices. A successful PAL write or drain does not, by itself, measure acoustic pressure at the speaker; hardware acoustic-loopback qualification would need a calibrated external capture fixture.

## Targets

| Platform | Target |
| --- | --- |
| macOS | `//projects/e2e/targets/cc_binary/pal-audio:desktop_test` |
| Chromium/WASM Worker | `//projects/e2e/targets/pkg_tar/pal-audio:browser_test` |
| iOS Simulator | `//projects/e2e/targets/ios_application/pal-audio:ios_pal_audio_simulator_test` |
| Android Emulator | `//projects/e2e/targets/android_binary/pal-audio:android_pal_audio_simulator_test` |
| ESP32-S3 AMOLED | `//projects/e2e/targets/h2loader_tar_zlib/pal-audio/amoled:package` |
| BK7258 | `//projects/e2e/targets/h2loader_tar_zlib/pal-audio/bk7258_v3_202405:package` |

The iOS App links the built Swift Package/XCFramework binary, and the Android App links the AAR binary; the Android runner compares the AAR and APK library bytes. Both mobile runners also inject a tracked per-track allocator, require allocation failure to reject track creation, then write/drain/close a successful track and verify every caller-owned allocation was freed. For boards, install only through H2Loader `send --file` and `reboot upgrade`, then independently `reboot app --monitor` and require the same complete case ledger, `confirm=0`, intact Partition 1 Loader, empty Stage and unchanged coredump. Device launchers replay the stored terminal case ledger while idle because H2Loader control frames can interrupt a serial log line. Do not erase an existing coredump.

`h2_pal_audio_decoder.h` is a separate PAL capability with a separate session and packet lifecycle; it is deliberately outside this Audio device App and must receive its own E2E gate. The source-to-case inventory in `app/api_coverage.json` is checked against the public header, while runtime evidence and compiler coverage are recorded separately in `qualification.json`.

The original six-platform runs and compiler coverage remain bound to their `qualification_snapshot_commit` and recorded `source_receipts`. PR review fixes have separate current-source receipts and fresh macOS, Chromium and iOS package runs in `review_fix_validation`; unchanged Android and board observations are not relabeled as new builds. The all-PASS path closes its track before terminal restore, while deterministic failure injection covers the corrected incomplete-close path.

Board launchers leave the success replay loop only for a qualified, confirmed run. Qualification or confirmation failure requests the existing serialized H2Loader return-to-P1 flow; if it returns, a full device reset releases any unrecovered audio resources. Host tests execute both actual board runners through failed recovery/configuration/confirmation paths and require reset instead of replay; native package builds validate SDK wiring. These tests do not claim physical fault injection.
