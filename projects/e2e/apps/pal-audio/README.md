# PAL Audio E2E

`pal-audio` is an independent portable App for the Audio PAL in
`libs/pal/include/h2/pal/hal/h2_pal_audio.h`. Its 24 mandatory cases cover all
11 Audio provider operations and all five track operations. The suite exercises
argument and format boundaries, real PCM capture and playback, volume and
microphone gain changes before and during capture, track drain/close, repeated
resource opening, and at least 30 seconds of simultaneous record/playback.
Every launcher passes a borrowed **real platform Audio PAL** directly; the
Board Runtime exists for time and H2Loader commands, not as a substitute for
the provider under test. A missing capability is `BLOCKED` and prevents the
qualified verdict.

The App reports the count, peak and sum of squared magnitudes for returned
microphone PCM, plus generated output PCM peak, frame count and measured soak
duration. Desktop additionally witnesses samples delivered through PortAudio's
real-device callbacks. Browser testing uses Chromium's deterministic fake
microphone source through the production `getUserMedia` and AudioWorklet PAL;
it does not claim a physical browser microphone. The mobile runs use simulator
audio devices. A successful PAL write or drain does not, by itself, measure
acoustic pressure at the speaker; hardware acoustic-loopback qualification
would need a calibrated external capture fixture.

## Targets

| Platform | Target |
| --- | --- |
| macOS | `//projects/e2e/targets/cc_binary/pal-audio:desktop_test` |
| Chromium/WASM Worker | `//projects/e2e/targets/pkg_tar/pal-audio:browser_test` |
| iOS Simulator | `//projects/e2e/targets/ios_application/pal-audio:ios_pal_audio_simulator_test` |
| Android Emulator | `//projects/e2e/targets/android_binary/pal-audio:android_pal_audio_simulator_test` |
| ESP32-S3 AMOLED | `//projects/e2e/targets/h2loader_tar_zlib/pal-audio/amoled:package` |
| BK7258 | `//projects/e2e/targets/h2loader_tar_zlib/pal-audio/bk7258_v3_202405:package` |

The iOS App links the built Swift Package/XCFramework binary, and the
Android App links the AAR binary; the Android runner compares the AAR and APK
library bytes. For boards, install only through H2Loader `send --file` and
`reboot upgrade`, then independently `reboot app --monitor` and require the
same complete case ledger, `confirm=0`, intact Partition 1 Loader, empty Stage
and unchanged coredump. Device launchers replay the stored terminal case ledger
while idle because H2Loader control frames can interrupt a serial log line.
Do not erase an existing coredump.

`h2_pal_audio_decoder.h` is a separate PAL capability with a separate session
and packet lifecycle; it is deliberately outside this Audio device App and
must receive its own E2E gate. The source-to-case inventory in
`app/api_coverage.json` is checked against the public header, while runtime
evidence and compiler coverage are recorded separately in `qualification.json`.
