# Board manufacturing acceptance

Each physical board has one firmware entry at `projects/mfg/apps/<board_name>`.
Build `:package` in that directory. These are H2Loader-role images with a
portable acceptance session, not PAL/GizClaw E2E runners.

```sh
source /path/to/firmwares-devenv/export.sh
bazel build --noannounce_rc --config=esp32s3 \
  //projects/mfg/apps/waveshare_esp32s3_touch_amoled_1_75c:package
```

Use `--config=esp32s31` for `esp_mosaico`, or `--config=esp32p4`, `--config=bk7258`, or `--config=ac791n` for the matching architecture. Other ESP32-S3 entries are `amoled`, `devkit`, `szp`, and `waveshare_esp32s3_a7670e_4g`. The remaining entries are `waveshare_esp32p4_wifi6_touch_lcd_4_3`, `bk7258_v3_202405`, and `jieli_ac791n_devkit`. JieLi's native package build requires a Linux host.

Physical acceptance recorded on 2026-10-30:

| Board | Recorded result | Remaining acceptance |
| --- | --- | --- |
| Waveshare 1.75C | 10/10 passed | Absolute optical, IMU and acoustic metrology are outside MFG scope. |
| AMOLED | 10/10 passed | BLE used a physical 1.75C peer, whose exact original firmware was restored. |
| SZP | 10/11 durable results; corrected camera preview accepted by the operator | The camera confirmation was not captured in device storage before unplugging. |
| Waveshare P4 4.3 | 9/10 passed | Camera acquired live frames; image quality and direction still require operator confirmation. |

These records describe the tested firmware versions. They do not claim that all boards were flashed with the current source, or that host tests qualify physical peripherals. DevKit, A7670E, BK7258, JieLi and ESP-Mosaico have MFG entries but no complete physical acceptance of their current GizOS images recorded here.

Every profile with both microphone and speaker checks also requires a separate,
operator-confirmed AEC check. This includes 1.75C, AMOLED, SZP, P4, BK7258,
and JieLi. The model rejects a duplex-audio profile which omits AEC or attempts to
confirm it automatically. DevKit and A7670E have no built-in duplex audio in
their profiles and omit these audio checks.

AMOLED's MFG entry explicitly enables `H2_AMOLED_BOARD_ENABLE_QMI8658` on
the board component and links the QMI8658 driver. The base BSP keeps this
provider opt-in; declaring an IMU step alone does not enable it. MFG 0.1.8
fixes the missing opt-in in the previous AMOLED package. Existing acceptance
results remain valid; retry a previously failed IMU step after updating.
MFG 0.1.10 maps this board's QMI8658 axes to the portrait tilt-ball display:
screen X is negative native Y and screen Y is native X. Raw PAL coordinates
and the requirement for at least 400 mg variation on both native axes are unchanged, so the
profile revision and existing acceptance results remain valid.

SZP's 320x240 screen has an FT6336 capacitive touch controller. MFG 0.1.11
connects its PAL Touch provider through Espressif's FT5x06 driver and enables
PASS/FAIL/RETRY on screen. The board uses the shared I2C bus (SDA 1, SCL 2),
address 0x38, with native X mirrored before swapping X/Y for landscape.
SZP profile revision 3 appends a required five-target touch check after the
unchanged nine revision-2 checks, preserving their recorded results.
MFG 0.1.13 maps SZP's tilt-ball coordinates into this landscape viewport:
screen X is native accelerometer Y, and screen Y is native X.
Raw PAL coordinates and both-axis movement thresholds remain unchanged.
MFG 0.1.14 includes SZP's board volume curve: 70% selects DAC register 0xA4
(-13.5 dB), replacing the former register-linear 0x7B (-34 dB). The maximum
remains 0xB0 (-7.5 dB), microphone/reference gains are unchanged, and the
speaker check still requires the operator to hear an undistorted tone.
MFG 0.1.15 raises SZP's speaker check to 100% and its DAC ceiling to 0xBF
(0 dB). The existing curve now attenuates below unity. Playback startup
verifies the PA output/configuration bits and logs actual DAC/mute/DRC
registers. Microphone and reference gains are unchanged. Audible loudness,
distortion and AEC still require their physical acceptance checks.
MFG 0.1.16 includes an ES8311/ES7210 I2S input initialization fix. Before
allocating channels, the shared provider restores DIN to input and verifies
its output driver is disabled. SZP's reboot hook drives GPIO12 low, and
retained output enable previously produced all-zero raw microphone frames
after software reboot or managed update. Existing gains, AEC processing,
MIC acceptance thresholds and recorded checks are unchanged.
MFG 0.1.17 connects SZP's GC0308 camera through esp32-camera 2.1.8. Profile
revision 4 appends camera as check 11, preserving the ten revision-3 results.
It captures actual 320x240 YUV422 frames and displays the shared live preview.
Three valid frames are prerequisites; the operator must move an object and
confirm that the preview follows it with correct colors and orientation.
The provider borrows the existing I2C bus, controls PCA9557 IO2 standby, uses
the supplier's DVP pins and 24 MHz XCLK, and normalizes GC0308 YUYV to PAL
UYVY while each frame is borrowed. Capture has the SDK's 4000 ms timeout;
stop rejects a live frame loan. Returning a frame restores its native bytes.
MFG 0.1.18 caps camera DMA buffering at 16 KiB to fit alongside the active
display and Loader transports, and handles the SDK's own failed-init cleanup
without double-deinitializing an already freed camera.
SZP's general PAL allocator explicitly uses PSRAM for Runtime/UI/audio buffers;
internal and DMA allocations use their separate allocators. Both camera
framebuffers are in PSRAM and the provider verifies each acquired address.
The SDK's ISR-visible queues, descriptors and internal camera task stack
retain their required internal memory. Camera startup logs PSRAM and DMA heap
availability so this allocation policy can be checked on the physical board.
MFG 0.1.19 preserves the source aspect ratio in the 160x90 preview viewport,
centering the complete image with black borders. SZP's 320x240 source becomes
120x90 with 20-pixel borders on either side. Preview placement leaves the
instructions below the image on SZP's 320x240 display.

Install the package with the normal H2Loader CLI `send --file <package>` and `reboot upgrade`. Loader self-update completes before the interactive session starts. Existing UART and BLE Loader commands stay available on the profiles that enable them; ESP-Mosaico uses the existing USB CDC and BLE management services. Loader recovery/update remains usable with unfinished or failed checks. App installation/launch requires all required checks to be PASSED.

## ESP-Mosaico

```sh
bazel build --config=esp32s31 //projects/mfg/apps/esp_mosaico:package
```

The revision-4 profile has eleven checks: combined display/brightness, nine evenly spaced app-defined touch points, original button cycles, staged AEC, battery, IMU, magnets, held vibration, Wi-Fi, BLE and camera. The shared model and AEC thresholds/phase logic remain unchanged. Explicit failure stays selected and required checks must all pass. The configured completion screen adds a deliberate press/release retest button using the existing transactional `reset all`: results are cleared durably and the Loader's in-memory acceptance gate is refreshed before starting again.

Mosaico's app entry connects the public factory network before starting Loader. Shared Wi-Fi only observes existing STA state. The entry also owns compensated-magnet validity/baselines and live indication; shared UI reads a borrowed observation callback. An app-local IMU adapter preserves real valid magnetic deltas while freezing invalid/recovery samples for the original shared min/max check. Its forwarded axes are diagnostic relative values in mGauss, not absolute field measurements. Provider errors and all-invalid readings remain errors. No SDK or BSP implementation is copied.

Shared additions include immutable presentation config, app-defined touch points, optional acceleration/gyro response checks based on reported/required channels, held vibration and asynchronous camera ownership. Wi-Fi rendering consumes the snapshot from the existing status poll to show the actual SSID and IPv4 without changing readiness or connection policy. Held-vibration presentation shows the button without overlapping hints; timed-vibration defaults retain their instructions. Battery shows presence, voltage and percentage; no current/charging stage is added.

BLE reuses actual Loader command round-trip proof. Use an existing physical peer package configured for the DUT UID. A BLE connection must obtain the current ticket and execute `mfg probe ble <ticket>` over BLE; discovery or serial confirmation is insufficient.

AEC uses the original QUIET/ECHO TEST/QUIET AGAIN/NEAR/DOUBLE-TALK/replay flow, measured windows and thresholds. Mosaico provides 16 kHz mono processed audio with 512-sample frames and image-local `H2_MOSAICO_AUDIO_AEC=ON`. The app adds no separate preparation stage: it uses the shared phase warmup directly. The earlier private preparation and its dedicated test have been removed. Its app sets the `mfg/aec` track amplitude to 250/1000 for both probe and voice replay. Codec phase volume remains the shared 100% request, microphone gain remains 18 dB/ADC `0xC8`, and observers see actual PCM. Board audio initialization, codec clock coefficients and the common provider remain unchanged from main; the earlier private DAC OSR experiment has been removed. Repeated double-talk remains the original behavior. These image-specific levels do not qualify other output levels or independent-source calibration. Temporary provider/app measurement instrumentation is excluded from the PR.

Both MFG and ordinary Loader are alternative `role=h2loader` images using the existing S31 layout. The ordinary Loader and its NULL install-display callbacks are unchanged. Image-local VFS capacity supports camera/CDC/filesystems. The twelve-check pass recorded for head `2a3279c9` belongs to the earlier revision-2 flow and does not certify this revision-4 image. The instrumented revision-3 image and cleaned revision-4 commit `8bedf532` each completed all eleven checks. The latter still contained the private DAC OSR override. After removing it, a local comparison passed quiet stability and far echo reduction but eventually failed double-talk headroom. Removing the extra app preparation also passed both quiet measurements and far echo reduction (17 dB), but double-talk eventually failed headroom. These comparisons do not establish either addition as necessary; the reduced PR requires fresh exact-image acceptance. Repeat hardware acceptance, completion-page reset/persistence and managed update/recovery checks.

## Operator flow

The board profile lists its checks in execution order. With the default interaction policy, wait for green PASS on a display board and short-press its first listed button to confirm the current check. ESP-Mosaico instead selects touch navigation and uses AI/BOOT only as measured inputs. On a board with Touch, opening the acceptance screen also opens the touch provider, including when a cold boot resumes a later or failed check. PASS, FAIL and RETRY accept touch input. FAIL records a failure; RETRY starts that check with fresh observations. The serial interface also works on boards without displays.

For the 1.75C:

1. RAM pattern and filesystem readback run automatically. This tests a 64 KiB
   allocation and a dedicated 512-byte temporary file; it is not an exhaustive
   Flash/PSRAM capacity or destructive storage test.
2. Inspect the red/green/blue/white quadrants and full/half/full brightness.
3. Touch and release all five crosses, using at least five distinct presses.
4. Press and release BOOT and PWR three times each. A subsequent BOOT press
   confirms; the final test release cannot confirm itself.
5. Tilt left/right and up/down, checking the moving ball. Both axes must vary
   by at least 400 mg. This board's panel axes are native Y/X, both positive,
   as physically confirmed on 2026-10-29. The PAL axes remain native.
6. Confirm the 440 Hz speaker tone is audible and undistorted: half a second
   of sound every two seconds, at 100% volume on 1.75C. Its DAC ceiling is
   unity gain (`0xBF`), raised from the original -7.5 dB (`0xB0`) ceiling.
   SZP and P4 also request 100%; other boards use the default 70% check volume.
   The previous volume is restored
   when leaving this check.
7. Speak near the microphone and check that the live LEVEL bars rise with
   speech and fall in silence. Confirm only when they respond. FRAMES counts
   captured PCM; a green PASS means the automatic prerequisites are ready
   for operator confirmation. The check requires
   captured PCM, nonzero energy and a minimum peak. It does not certify AEC,
   near-end intelligibility, or double-talk performance.
8. Connect Wi-Fi through the Loader command and wait for a usable IP address.
   The firmware contains no network credentials.
9. Connect a **second development board** to this board's Loader BLE service.
   Query MFG status there, then send `h2loader mfg probe ble <ticket>` over
   that actual BLE connection. A serial probe is rejected. No Mac radio is
   needed. This is a Loader GATT command round trip, not the full radio E2E
   suite.
10. Follow the separate **AEC** check. Remain quiet through QUIET, ECHO TEST
    and QUIET AGAIN. The board plays the single-board 500/1000/2500 Hz probe
    at 100% volume and uses actual pre/post-AEC and DAC observations. Each
    measured window needs 96 complete capture/DAC frames, real-time cadence,
    active reference, no clipping and at least 10 dB of background-subtracted
    far echo reduction. Background must be stable and the real echo must
    exceed it by at least 10 dB.
    At SPEAK NOW, speak near the mic; keep speaking at SPEAK OVER TONE.
    Those stages repeat while activity is insufficient. LISTEN TO VOICE
    replays about three seconds of the actual AEC output recorded during
    simultaneous playback and speech. Confirm only if your voice is clear
    and the far test tone is suppressed. The recording stays in RAM and is
    erased on leaving the check.

On 1.75C, AMOLED, SZP and P4, the AEC check combines automatic single-board
echo/headroom evidence with operator judgement of processed speech. It does not grant controlled
independent-source calibration, measured near/double-talk retention, or
unrestricted voice qualification. The existing `projects/e2e/apps/aec-calibration`
runner retains those stronger fixture requirements. MFG does not retune or
persist microphone or AEC gains. The board's unity DAC ceiling is a BSP
configuration change and affects all Apps built against this board.

### AEC on the other audio boards

The shared check uses each board's actual 16 kHz mono frame size rather than
requiring the 1.75C's 512-sample frames. Measurement and warmup lengths keep
the same approximately 3.1-second and 1.0/3.1-second durations. SZP's BSP now
enables its existing ES7210 reference/AEC path with the 512-sample ESP-SR
processing frame; its previous 256-sample configuration had disabled AEC.

BK7258 and JieLi already run AEC but currently expose only the
processed microphone PCM, without the full raw/reference/DAC observer.
Their MFG AEC check therefore uses operator acceptance:

1. Follow QUIET, ECHO TEST and QUIET AGAIN while the board plays the far probe.
2. Speak at SPEAK NOW, then keep speaking at SPEAK OVER TONE.
3. At LISTEN FOR ECHO, listen to the actual far-only AEC output recording.
   The test tone should be suppressed.
4. At LISTEN TO VOICE, listen to the actual double-talk AEC output recording.
   Voice should remain clear while the far tone is suppressed.
5. Confirm only after both recordings have played and both sound correct.
   FAIL and RETRY remain available. The two recordings repeat for listening.

This path requires the provider to report real AEC enabled, complete processed
PCM windows, real-time capture cadence, no observed output clipping, audible
near activity and downstream playback drain. Partial SDK reads are assembled
into complete windows. A missing/disabled AEC path or a diagnostics error does
not become a successful operator check. Only an explicit UNSUPPORTED
diagnostics response selects this path. Muted output cannot complete the
speech phases.

The screen shows LISTEN FOR ECHO instead of an invented dB measurement.
Serial windows identify `mode=operator raw_observed=0 dac_observed=0`, and
`echo_db=-1` means unmeasured. Queued/drained audio is not reported as observed
physical DAC frames. This is manual functional acceptance; it does not grant
the quantitative pre/post qualification or gain-calibration certificate.
Both short recordings remain in RAM and are erased on exit.

## Controlled gain calibration

The 1.75C also has a separate `:calibration_package` entry in its board
directory. It calls the existing, unchanged
`//projects/e2e/apps/aec-calibration/app:aec_calibration` runner. This entry is
a temporary Loader image with normal H2Loader recovery and update; it never
marks manual MFG checks passed. It accepts one gain pair per job, keeping the
command service available and rejecting installation/reboot during a job.

AMOLED provides `:calibration_peer_package`, an App which plays the runner's
actual near probe through its physical speaker. Build it for the intended
DUT identity:

```sh
bazel build --noannounce_rc --config=esp32s3 \
  --define=H2_MFG_PEER_DUT_UID=<12-digit-DUT-UID> \
  //projects/mfg/apps/amoled:calibration_peer_package \
  //projects/mfg/apps/waveshare_esp32s3_touch_amoled_1_75c:calibration_package
```

Keep both boards' original packages and identities. Install the source App
only in the peer's App partition, preserving its original Loader. The source
uses its saved test Wi-Fi without modifying those credentials; READY requires
an actual IPv4 address. Connect the DUT Loader to that same test network.
Leave the boards in fixed positions and record the fixture geometry/levels
with the results. The Mac is a serial controller and log sink.

The source fixture uses 95% volume and settles its physical output for two
seconds before acknowledging START. This is recorded as source-control time;
the runner's DUT warmup, measurements and acceptance thresholds are unchanged.
Each source span logs accepted playback-queue frames and elapsed time. These
are producer cadence observations, not hardware DAC/drop counters; the DUT
also checks the actual near input's spectral stability.

After a managed DUT Loader update, issue `reboot loader --monitor` and wait
for the exact image's `H2_MFG_CALIBRATION_READY` before queuing a job.

Start the peer's serial `monitor`, then request a fresh run on the DUT:

```sh
h2loader --no-ble --port <DUT-port> mfg calibrate \
  <peer-IPv4> 100 100 <fresh-16-hex-run> <12-digit-peer-UID> --monitor
```

The first percentage is speaker volume and the second is microphone gain.
On 1.75C, 100% mic uses ES7210's physical maximum **37.5 dB** PGA setting.
The integer configuration represents that top setting as 38. From MFG 0.1.6,
95% correctly selects 36 dB; older mapping selected 34.5 dB for that request,
so those historical measurements must retain their actual hardware meaning.
The board's DAC ceiling is unity gain, and the ADC reference stays at
12 dB. Register observations from stopped/joined windows report the real
DAC and ADC control bytes. Calibration restores the original controls after
each job.

Each job keeps the existing runner's two probe amplitudes, six distinct
frequency bins, background/far/near/double-talk/stability phases, 64-frame
warmup, 96-frame measurements and 640-frame stability windows. Every window
checks actual complete ADC/AEC/DAC observations, real-time cadence and
headroom. The unchanged public report verifier must accept the complete log;
an emitted summary or a low total output level alone cannot grant a pass.
Match every source request to the physical peer's nonce, sequence, UID,
probe checksum and successful start/stop acknowledgement.

The peer accepts increasing nonzero sequences within each run and replays an
acknowledgement only for an exact copy of the latest request. Delayed controls
from earlier sequences or retired run nonces cannot change playback or extend
its lease. Each new job needs a fresh nonce, starts at sequence 1, and requires
the previous source to be stopped. Shutdown continues the last job's sequence.
The peer retains up to 256 run nonces per boot and rejects further new runs
without evicting old entries; restart the stopped peer before another batch.

Search speaker volume first, then microphone gain within that volume.
Compare hardware gain steps, preserve failed candidates and repeat boundary
and selected settings. Results apply to the recorded probe amplitudes and
fixture, not an unrestricted voice or absolute loudness specification.
Do not silently reinterpret a failed run as passed.

When finished, use `mfg source-shutdown --monitor` on the DUT and require the
peer's `H2_MFG_AEC_PEER_END ... cleanup=0 retained=0` before restoring its
original App. Restore the DUT's formal MFG Loader using the calibrated board
configuration. Repeat the audio checks affected by new defaults; preserve
other accepted checks. Verify exact firmware identities, empty Stage, zero
last result, blank coredump and released serial ports on both boards.

Audio-board profile revision 2 explicitly appends AEC to each unchanged
revision-1 prefix: nine checks for 1.75C/AMOLED, eight for SZP/P4/BK7258, and
seven for JieLi. All previous status digits, including failures, are preserved;
only AEC starts UNTESTED. Other unknown revisions follow reset rules. AEC
remains required before App startup.

AMOLED, DevKit and 1.75C also provide a peer App in the same board directory:

```sh
bazel build --noannounce_rc --config=esp32s3 \
  --define=H2_MFG_PEER_DUT_UID=b81f3fcad4fa \
  //projects/mfg/apps/amoled:peer_package
```

Use the intended DUT's 12-digit lowercase BLE UID from Loader `status`.
The other supported peers are `//projects/mfg/apps/devkit:peer_package` and
`//projects/mfg/apps/waveshare_esp32s3_touch_amoled_1_75c:peer_package`.
The peer merges ADV/SCAN_RSP from that identity, connects to Loader, verifies
the authoritative UID, sends only the BLE probe, and reads back the committed
result at the BLE index reported by MFG, including when later checks such as
AEC remain untested. It never confirms manual checks. Completion emits
`H2_MFG_PEER_PASS ... durable=1` and `H2_MFG_PEER_END ... code=0`.
Keep the peer's original package and restore it after bench use.

The profile for a board without built-in audio omits speaker/microphone tests.
DevKit tests its RGB LED. A7670E tests the modem identity response; it does not
register a product or place a call. JieLi lists all eight ADC keys; BK reads
its three keys through their radio-button group.

P4 MFG 0.1.20 adds GT911 Touch PAL and OV5647 MIPI-CSI Camera PAL. Profile
revision 3 appends Camera after the nine revision-2 checks; those nine results
are preserved when migrating from revision 2. Its ten checks are storage,
display, touch, BOOT, speaker, microphone, Wi-Fi, BLE, AEC and camera.
The speaker check requests 100% volume.

Touch uses the display's native 480x800 coordinates, GPIO23 reset, and probes
both GT911 addresses (0x5d and 0x14). Touch, camera and ES8311/ES7210 audio
borrow one board-owned I2C bus on SDA7/SCL8. Closing one provider does not
delete the bus used by the others.

Camera uses pinned esp_video 2.5.0 with the OV5647 1280x960 RAW10 binning
mode. The P4 ISP supplies UYVY BT.601 limited-range frames, matching the
shared MFG preview converter. Two 128-byte-aligned framebuffers are explicitly
allocated in PSRAM. Every exported address and frame length is validated.
Dequeue has a 1000 ms timeout. Live loans block stop; failed requeue or cleanup
retains ownership for retry. Board teardown and reboot stop capture first.
P4's general allocator and ISP controller task stack use PSRAM; dedicated
internal/DMA allocators retain their separate behavior.
MFG 0.1.21 fixes P4 Loader and App command identity initialization: P4 uses
its factory eFuse MAC because it has no local Bluetooth MAC. Other ESP
targets retain their existing Bluetooth-MAC identities.
MFG 0.1.22 reserves 16 VFS registrations on P4 so CSI/ISP devices fit
alongside Loader filesystems and transports. GT911 reset now completes before
probing its two addresses, preventing reset from invalidating the selected
address. `mfg retry <step> <ticket> --monitor` captures startup diagnostics
through the same serial session.
MFG 0.1.23 fixes P4 BOOT to the schematic's GPIO35 (active low). Its button
page includes all three physical keys: BOOT down/up counts, POWER off/on and
RESET restart/resume. POWER connects to the independent U20 key controller;
RESET connects to ESP_EN. Their physical actions require operator confirmation
and are not fabricated GPIO counts. Test POWER and RESET first, then complete
three BOOT cycles and confirm the complete button check.
MFG 0.1.24 uses 512-sample/32-ms P4 audio frames to match the pinned ESP-SR
direct AEC library. The former 256-sample configuration was rejected during
AEC initialization and made the entire lazy audio provider unavailable,
including the speaker check. AEC remains enabled.
When the C6's on-air Bluetooth address differs from that P4 UID, build the
physical BLE peer with `--define=H2_MFG_PEER_DUT_BLE_ADDRESS=<12-hex-address>`
in addition to `H2_MFG_PEER_DUT_UID`. The address selects advertisements;
the connected Loader's authoritative UID must still match before any probe.
Set that address to `000000000000` to discover Loader service candidates when
the C6 address is unknown; each connection must still match the supplied DUT
UID before it can probe or commit BLE acceptance.

Build with `--config=esp32p4` and
`//projects/mfg/apps/waveshare_esp32p4_wifi6_touch_lcd_4_3:package`.
Use a physical board peer for BLE acceptance and restore its original image
afterward. Camera requires three valid frames and operator confirmation of
live motion, colors, orientation and proportions. Board entries and successful
compilation do not establish physical acceptance.

## CLI and persistent results

Build the CLI from the same checkout to recognize the optional MFG
availability bit:

```sh
bazel build --noannounce_rc //projects/h2loader/targets/cc_binary/cli:h2loader
h2loader --no-ble --port /dev/cu.usbmodemXXXX mfg status
h2loader --no-ble --port /dev/cu.usbmodemXXXX mfg confirm display 1234abcd
h2loader --no-ble --port /dev/cu.usbmodemXXXX mfg fail display 1234abcd
h2loader --no-ble --port /dev/cu.usbmodemXXXX mfg retry display 1234abcd
h2loader --no-ble --port /dev/cu.usbmodemXXXX mfg reset all 1234abcd
```

Replace the step and eight-digit hexadecimal ticket with the current values from `mfg status`. Tickets change on each transition and each boot, so delayed confirmations cannot pass another check. `probe` is restricted to a real BLE command round trip on profiles that require it; ESP-Mosaico uses the same BLE proof. Other actions use the enabled Loader transport(s). `skip` records SKIPPED and continues, but a skipped required check still blocks App work. Use `retry` to revisit it.

`H2_MFG_STATUS` reports the selected check, readiness, error, ticket, total,
completion and status digits. Digits are 0=UNTESTED, 1=PASSED, 2=SKIPPED,
3=FAILED. `result=OK` acknowledges the command; `complete=1` alone means every
required check is passed. A completed record is resumed across reboots.
Each result is committed through the existing H2Loader MFG preferences before
the next check starts. Storage errors are returned rather than silently
advancing.

A completed display board shows its summary again after a cold boot when
there is no App to launch. Pending Loader replacements bypass the interactive
session and the App acceptance gate, so an unfinished check cannot prevent
repairing the factory Loader.

Changing a profile's check semantics/order requires incrementing its
`revision`. This invalidates the previous acceptance record. Shared state,
hardware and UI live under `projects/mfg/libs/mfg`; platform entry points and
board policies remain directly under `apps/<board_name>`.

BK/JieLi entries override the public Loader board extension hooks. They reuse
the maintained transport, partition and recovery implementations. Before
installation/reboot, MFG stops polling, closes the audio track before the
speaker, joins capture/playback, and closes touch/display. Failed cleanup
retains handles and blocks the transition for retry.

## Focused validation

```sh
bazel test --noannounce_rc \
  //projects/mfg/libs/mfg:model_test \
  //projects/mfg/libs/mfg:session_test \
  //projects/mfg/libs/mfg:audio_test \
  //projects/mfg/libs/mfg:aec_test \
  //projects/mfg/libs/mfg:touch_resume_test \
  //projects/mfg/libs/mfg:peripheral_checks_test \
  //projects/mfg/libs/mfg:completed_display_test \
  //projects/mfg/libs/peer:scan_test \
  //projects/h2loader/libs/h2loader:h2loader_command_test \
  //libs/h2loader_host:h2loader_host_test \
  //libs/h2loader_host:esp_boot_policy_test \
  //libs/h2loader_host:esp_slip_test \
  //projects/h2loader/apps/cli/app:h2loader_cli_app_test
```

Model tests cover readiness, stale tickets, failed persistence, resume,
failure/retry/reset, mandatory AEC for duplex audio and the strict all-PASSED
gate. Session tests use real
locks and SQLite preferences to exercise BLE transport proof, recovery
bypass, handoff while the operation mutex is held, and failed join retention.
Optical and acoustic results require an operator on the real device.
Audio tests simulate a 125 ms display iteration against a 32 ms PCM consumer
to detect playback starvation, verify that pending microphone frames are
drained, and check volume restoration and failed track-close retention.
AEC regressions cover mandatory human speech/replay, missing reference,
missing echo reduction, clipping, a muted output and failed cleanup retention.
They exercise 256/320/512-sample frames, partial processed-PCM reads, both
operator recordings, disabled AEC, diagnostics errors, missing capture and
failed downstream drain.
Touch resume regression checks the two screen controls after a cold boot
directly into a failed AEC step without entering Touch first.
