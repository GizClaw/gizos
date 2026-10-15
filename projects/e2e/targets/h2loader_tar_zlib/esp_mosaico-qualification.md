# ESP-Mosaico E2E qualification

Current checkpoint (2026-10-15): **rebased onto main `68e867d0`; current-baseline requalification in progress**. The 2026-10-14 R105 WebRTC, R84 Loader and final Flash restoration receipts remain historical, bound to their original sources and immutable artifacts. PR #709 stays Draft and GizClaw remains excluded. Rebase does not transfer old hardware qualification to the new binaries.

The mainline retirement of the mixed PAL App also retires Mosaico's legacy `pal-pref` launcher. Preference coverage belongs to the existing independent `pal-storage` contract, including its cross-boot persistence and cleanup checks. No compatibility label or copied old App is retained.

## Rebased baseline checkpoint (2026-10-15)

The new baseline is `68e867d0`. Fresh immutable artifacts are R106 (suite/Loader/recovery), R107/R109 (default-off WebRTC performance), R110 (instrumented WebRTC diagnosis), DevKit R21 (Lua peer), and R108 (board diagnostic). Source manifests include the exact pre-commit diff; subsequent host-test and diagnostic-only edits do not relabel the earlier artifacts.

| Validation | Current-baseline evidence |
| --- | --- |
| Focused host contracts | 104/104 PASS |
| Complete macOS host test selection | 923 PASS, 23 incompatible-platform skips; the original LVGL allocator-accounting race is fixed and the actual display test passes 100 consecutive runs |
| Native builds | R106 17 roots, R107/R109/R110 performance, DevKit R21 peer and R108 diagnostic built and archived with verified hashes |
| Display | R106 two independent boots, 24/24 each; driver-transfer and lifecycle evidence, no new optical claim |
| Core | R106 two independent boots, 41/41 each, task-resource probe, cleanup, confirmation and wall-clock restoration PASS |
| Storage | R106 36/36 across phases 1/2/3/4/4, independent reboots, empty completion rechecks and unchanged Loader/crash storage PASS |
| Lua Link | R106/R21 bilateral five rounds, real DevKit USB cycle (`lost:-10`, 4101 ms), both exact public Loader returns and unchanged crash storage PASS |
| Audio / Decoder | R106 Audio 24/24 and Decoder 29/29 on two independent boots each; actual PCM/I/O, 30-second audio soaks, cleanup/confirmation and public Loader returns PASS |
| Loader recovery | R106 original full 51/51 matrix PASS, including real HTTP peer witness and 20,960-byte induced-crash dump read/erase |
| WebRTC performance | NOT QUALIFIED. R107 second boot fails unloaded P99 288.856 ms; R109 controlled AP/channel comparison fails loaded P99 359.198 ms. Both exceed the unchanged 200 ms gate. R110 diagnostic boot passes but does not qualify default-off firmware. Actual public Loader returns verified |
| Board diagnostic / original Flash | R108 automatic Core baseline 8, Audio 24, Camera 17/40 frames, sensor and local radio checks PASS; operator RGBW/touch/49-frame preview/BOOT tones PASS. Valid left-slot insertion and final original 16 MiB restoration pending |

The first Display controller attempt timed out because it parsed an incomplete trailing replay JSON line. Its raw evidence is retained; the local controller now consumes complete lines and the new two-boot run passes the unchanged 24-case assertions. A full CI build also found the separate board diagnostic still depended on the retired mixed PAL App. That launcher now calls the independent Core App's existing eight baseline probes and uses its task namespace, retains the halt-on-live-cleanup behavior, and explicitly labels the dashboard as baseline coverage. It does not copy retired implementations or claim these eight probes satisfy the full 41-case contract. The complete macOS build now passes all 3,872 targets after also aligning the three newly reachable Lua task declarations with the mainline DevKit policy. This is build compatibility only; GizClaw business acceptance remains excluded. All build, test and coverage checks for code head `a237ec4c` pass; exact-head owner approval is still required.

### Current-baseline WebRTC investigation

R107 uses the original private AP fixture. Its first boot passes; its second boot has unloaded P99 288.856 ms with 97/100 frames, loaded P99 145.111 ms and fairness 1180. The Pion peer receives and echoes all 200 packets; one unloaded arrival gap at the peer is 230.148 ms, before echo processing. This locates part of the delay before the peer echo but does not distinguish device scheduling, SDK queues or the radio path.

A subsequent read-only observation finds the host on channel 11 and the R107 board pinned to channel 6. R109 uses a privately configured stronger channel-11 AP and waits for 30 seconds of low host build load. The host AP identity remains unknown; matching channels do not prove a shared AP. The complete measured R109 window has zero sampled build CPU, but unloaded P99 is 194.164 ms and loaded P99 is 359.198 ms, with 100/99 frames and fairness 1215. Thus neither the controlled fixture nor the absence of competing builds resolves the qualification failure. No thresholds, QoS markings, worker policies or shared workload assertions were changed.

R110 enables only the existing bounded worker, RTP-receive and UDP-mailbox diagnostics. Its one diagnostic boot has unloaded/loaded P99 105.574/155.928 ms, 200 authenticated received packets, no missing frames and fairness 1103. It also records 142 mailbox drops. Maximum per-packet decode time is 1.174 ms; maximum peer echo processing is 0.146 ms. The 174.618 ms connected-command aggregate is consistent with close-time printing of the 200 diagnostic records over 171 ms, after performance statistics; it is not evidence of a media-time command stall. Per-command opcode timing is absent, so exact attribution is not asserted. The diagnostic PASS cannot erase R107/R109 failures or qualify the non-instrumented image. Current evidence does not justify another speculative production tuning change or a claim that Wi-Fi is the sole cause.

### Upstream change and regression coverage

The rebase validation targets the integration boundaries changed by the 13 upstream commits. Unchanged historical suite receipts remain artifact-bound; they are not presented as fresh R106 passes.

| Upstream change | Mosaico validation |
| --- | --- |
| Mixed PAL App retirement | Remove the legacy Preference launcher; existing Storage 36-case, five-boot contract PASS. Board diagnostic reuses the independent Core baseline API and explicitly distinguishes its eight probes from the full 41-case qualification. |
| Positive ESP Time delay rounding | Host tick-boundary checks and fresh Core 41/41 on two boots PASS; fresh Audio 24/24 and Decoder 29/29 on two independent boots each PASS. |
| Runtime BLE name and retained GATT schemas | Runtime/provider host checks and fresh bilateral Lua five-round business plus real USB loss PASS. Optional product naming does not replace protocol board/target identity checks. |
| Loader SHA-256 reuse and boot metadata | Host tests and unchanged full 51-case hardware recovery/checksum/HTTP/crash matrix PASS. |
| H2Peer close and SCTP handshake semantics | Host lifecycle and association regressions PASS; R107 and R109 fail the existing audio-gap smoke threshold; R110 diagnostic observations are recorded separately without transferring qualification. |
| ESP archive rescan composition | Native firmware builds, host contract checks, complete macOS build and all 3,872 S31 analysis targets PASS. |

The source manifests for R106/R107/DevKit R21/R108 were reconciled against `a237ec4c`: differences are documentation, the LVGL host test, the separate standalone diagnostic, or the excluded GizClaw launcher. None changes the corresponding built image's inputs. The original manifests and pre-commit diffs remain the artifact identity; this comparison does not rename their source revision.

## Comparison with existing boards

The new launchers reuse the same portable Apps/device runners as DevKit or AMOLED.
BK7258 supplies an additional comparison for each shared suite, not transferable
hardware evidence. No other board's evidence files are copied. A missing provider,
BLOCKED/NOT_RUN case, failed cleanup or missing independent boot remains a failure
of admission; it must not be silently removed from the registry.

| Suite | Reference launcher | Mosaico entry | Recorded device status (artifact-bound) |
| --- | --- | --- | --- |
| pal-core | `pal-core/devkit` | [`pal-core/esp_mosaico`](pal-core/esp_mosaico/BUILD.bazel) | r32 41/41 on two independent boots; clock restoration, task probe, cleanup/confirmation, screens and managed install PASS |
| pal-storage | `pal-storage/devkit` | [`pal-storage/esp_mosaico`](pal-storage/esp_mosaico/BUILD.bazel) | r32 36/36 over five boots (1/2/3/4/4); real persistence, cleanup/confirmation, screens and managed install PASS |
| pal-crypto | `pal-crypto/devkit` | [`pal-crypto/esp_mosaico`](pal-crypto/esp_mosaico/BUILD.bazel) | r32 22/22 on two independent boots; cleanup/confirmation, screens and managed install PASS |
| pal-json | `pal-json/devkit` | [`pal-json/esp_mosaico`](pal-json/esp_mosaico/BUILD.bazel) | r32 15/15 on two independent boots; cleanup/confirmation, screens and managed install PASS |
| pal-http | `pal-http/devkit` | [`pal-http/esp_mosaico`](pal-http/esp_mosaico/BUILD.bazel) | fresh r35 45/45 on two independent boots after host restart; real HTTP/TLS witnesses, cleanup/confirmation, screens and managed install PASS |
| pal-mqtt | `pal-mqtt/devkit` | [`pal-mqtt/esp_mosaico`](pal-mqtt/esp_mosaico/BUILD.bazel) | fresh r35 36/36 on two independent boots; exact broker/TLS/first-ledger witnesses, provider release/resources, delivery/confirmation, screens and managed install PASS |
| pal-net-tls | `pal-net-tls/devkit` | [`pal-net-tls/esp_mosaico`](pal-net-tls/esp_mosaico/BUILD.bazel) | fresh r35 core profile: 37 mandatory/38 total passes on two boots, one optional unsupported; exact peer/cleanup/confirmation/screens PASS; full-Net remains unqualified |
| pal-wifi | `pal-wifi/devkit` | [`pal-wifi/esp_mosaico`](pal-wifi/esp_mosaico/BUILD.bazel) | r33 39/39 on complete public managed install and independent reboot; each fresh 600-second AMOLED window has six ordered real leases, restoration/confirmation and preserved Loader/dump PASS; first interrupted attempt NOT ADMITTED |
| pal-webrtc | `pal-webrtc/devkit` | [`pal-webrtc/esp_mosaico`](pal-webrtc/esp_mosaico/BUILD.bazel) | fresh r36 43/43 on two independent boots; real Pion encrypted transport, typed fingerprint rejection, zero retained allocations, confirmation/screens and preserved Loader/dump PASS |
| pal-audio | `pal-audio/amoled` | [`pal-audio/esp_mosaico`](pal-audio/esp_mosaico/BUILD.bazel) | r32 24/24 on two independent boots; actual capture/output and >=30-second soaks, cleanup/confirmation/screens PASS; later r77 three-tone operator confirmation is separately artifact-bound |
| pal-audio-decoder | `pal-audio-decoder/devkit` | [`pal-audio-decoder/esp_mosaico`](pal-audio-decoder/esp_mosaico/BUILD.bazel) | r32 29/29 on two independent boots; cleanup/confirmation/screens and managed install PASS |
| pal-display | `pal-display/amoled` | [`pal-display/esp_mosaico`](pal-display/esp_mosaico/BUILD.bazel) | r32 24/24 on two independent boots; original DMA/pattern/dimming plus STARTING/CONFIRMING/PASS rendering PASS; operator stage/pixels observations retain their separate r31/r28 bindings |
| atomic | `atomic/devkit` | [`atomic/esp_mosaico`](atomic/esp_mosaico/BUILD.bazel) | r32 56/56 on two independent executions, all 20 workers joined; cleanup/confirmation/screens and managed install PASS |
| libco-smoke | `libco-smoke/devkit` | [`libco-smoke/esp_mosaico`](libco-smoke/esp_mosaico/BUILD.bazel) | r32 six phases/10,000 switches on ten independent boots; confirmation/screens, managed install and actual original dump preservation PASS |
| lua-link | `lua-link/devkit` | [`lua-link/esp_mosaico`](lua-link/esp_mosaico/BUILD.bazel) | r32 host/r6 AMOLED join: five full bilateral rounds, confirmed identities and connected hold; operator PWR cycle gives actual lost:-10 / 4100 ms / 1766 ticks, both exact public Loader returns and dump preservation PASS |
| pal-pref (retired) | historical `pal-pref/devkit` | historical `pal-pref/esp_mosaico`; current replacement: `pal-storage` | r32 fresh 31 operations over three boots plus independent empty 1 recheck; image reset/persistence/confirmation/screens PASS after missing-ledger interruption; only private fixture prepared using distinct archived r28 image |

| iperf | `iperf/amoled` | [`iperf/esp_mosaico`](iperf/esp_mosaico/BUILD.bazel) | r75/r19 two independent 14-case matrices and 28 actual peer witnesses PASS; artifact-bound |
| webrtc-performance | `webrtc-performance/amoled` | [`webrtc-performance/esp_mosaico`](webrtc-performance/esp_mosaico/BUILD.bazel) | r105 two independent original smoke boots PASS, diagnostics off, actual Pion default DSCP 0; fixed private board AP, source 6c0cb80f |
| gizclaw-e2e | `gizclaw-e2e/devkit` | [`gizclaw-e2e/esp_mosaico`](gizclaw-e2e/esp_mosaico/BUILD.bazel) | EXCLUDED by operator; no business qualification claimed |

PAL Core requires 41 cases covering 46 interface operations; the old board page's
8 Core cases cannot close it. Display requires the portable 24-case registry with
observations of completed DMA chunks; the observation is not panel readback or
optical verification. Shared suite READMEs and registries own the exact counts and
peer/cleanup requirements, including Storage's multi-boot persistence sequence and
MQTT's first-run ledger, broker/TLS witnesses and post-delivery confirmation.

## Core direct-flash investigation

The correctly selected OTA1 App built from `ef7eb77a` executed all 41 shared
Core cases: 40 passed and `pal.core.time.wall-set` failed with `-2000`
(UNCALIBRATED). Cleanup, the 4/16/64 KiB stack observations, allocation-failure
recovery and the 100-task resource probe passed. App SHA-256:
`1b1603a53c64a1269bded8105e30c0e194110ae0f76f183d14a50cd01d015937`.
This is a failed bring-up receipt, not final-source qualification or proof of
managed installation.

The wall-set case needs calibrated UTC to save and restore before testing its
write operation. The Mosaico launcher now verifies the cold UNCALIBRATED state,
uses the native SDK to establish a controlled fixture epoch, and runs the shared
suite unchanged. It then restores the raw boot clock plus elapsed monotonic time,
checks that PAL again reports UNCALIBRATED, and includes restoration in its READY
qualification gate. The fixture epoch is test data, not a claim of actual UTC.
The corrected source committed as `3ec6deda` passed 41/41 cases on the first boot
and a separate user-triggered RESET. Both executions reported `cleanup=0`,
`cold_boot_prepared=1 restore=0`, `task_probe=0` and `confirm=0`. The stack,
allocation-failure recovery and 100-task resource observations passed on both
boots. The flashed App was 1,574,848 bytes with SHA-256
`76f530981ba6897c4a91f74c1a8a5eee6bfdf426a7f8c8466e03208a6164885d`.
The build preceded the commit but contained exactly its launcher source; the
commit also added this qualification documentation. Separate startup/BOOT markers
identify the second execution; repeated ledgers alone are not reboot evidence.
This qualifies the observed Core assertions on this artifact, while managed
installation/recovery and the remaining suites still block overall acceptance.

## Remaining hardware and integration gates

- Board diagnostics: rerun display/touch/buttons, both magnetometers, BMI270,
  read-only battery, audio and camera on the final artifact. Observe actual sound
  and pixels separately from successful API calls. Camera insertion needs its real
  empty-slot-to-insert-to-capture sequence; live removal remains unsupported.
- Managed H2Loader transport: Loader and all 16 launchers now opt into a
  board-owned TinyUSB adapter. CDC0 carries diagnostic output and CDC1 carries
  the existing IO Stream iKCP protocol. The shared ESP H2Loader accepts a physical
  I/O override at startup; other boards retain their existing UART/USB Serial-JTAG
  defaults. All 16 dual-CDC packages build. Host callback/configuration tests,
  the public serial E2E status/identity case, and App-to-Loader return passed on
  hardware. The device reports UID `1c2904d0a629`, diagnostic CDC at interface 0
  and command CDC at interface 1 (USB VID:PID `303a:4002`). The first dual-CDC Core
  image (`cef6f7a41b839d50845a1ced1daa2ee3eea0c9c2028bf1f392623bc97621c5d4`)
  also passed 41/41 before the managed-install attempt.
  Managed installation reached `write_partition_2` but repeatedly reset;
  the host retry was stopped. This is a failed installation, not qualification.
  Unlike DevKit's 64 KiB entry task, the initial Mosaico Loader used the 16 KiB
  SDK main stack. The launcher now owns a 64 KiB PSRAM entry task and reports
  reset reason plus installation/launch stack headroom. The revised Loader
  resumed the preserved Stage and successfully wrote and booted Core. Minimum observed launch stack headroom was 41,628 bytes out of
  65,536: the observed 23,908-byte usage exceeds the original 16 KiB stack.
  The saved coredump partition was all 0xff, so no panic backtrace is available.
  A fresh JSON public managed-install E2E then passed, including all package
  bytes acknowledged, expected image identity, unchanged Loader and empty Stage.
  The unrelated USB-UART adapter must not be used.
- Install a qualified Loader first, record original partitions/coredump and verify
  Stage/package/image identity, managed upgrade, independent reboot, cleanup and
  recovery according to each suite. Preserve a full Flash backup before replacing
  the historical diagnostic image; bind subsequent receipts to exact source and
  artifact hashes.
- Network suites need a controlled AP and reachable TCP/TLS/HTTP/MQTT/WebRTC peers,
  trusted fixture configuration and peer witnesses. Never commit credentials.
  Existing scan/AP and BLE advertisement smoke do not prove these scenarios.
- BLE connection/GATT, right-slot function, motor, NAND, battery calibration and
  update/rollback remain unqualified. S31 AEC has no qualified binary ABI. Unsupported
  features require an explicit scope decision, not a synthetic PASS.
- `iperf`, `webrtc-performance`, `gizclaw-e2e` and storage-backup targets also exist
  on other boards; their workload/product/recovery prerequisites must be explicitly
  included or excluded before claiming every repository E2E applies to this board.
  Their Mosaico qualification is pending, not implicitly passed by this matrix.

## Managed-install receipts and Crypto configuration correction

These observations belong to the exact artifacts below, not to later rebuilds.
The Type-C management port was CDC1; CDC0 independently captured device ledgers.

- Loader image SHA-256: `392beed152935ef8808f701eac70aa8f1cc2b5a9bf8ffc7de5645b324115a4ea`.
- Core image SHA-256: `1040ec93a7c96404bff68b2663b1a05e781cb7df69f9b21f6690ad2bcbce7266`.
  Its preserved Stage resumed after the Loader-only ROM repair. Both first boot
  and an independent managed reboot passed 41/41, with cleanup, task probe,
  startup confirmation and clock restoration all zero. This is not a PASS receipt
  for the earlier interrupted host install session. Final status reported App
  partition 2, empty Stage and last result zero.
- JSON image SHA-256: `275864d35026d5c7bb4344f1e27922b02a9f59b5c5a25c4b4a1f538d9feddb1a`.
  Package SHA-256: `e2b6be40d97195a5a16b38f3922a2f919dd116b7eb5ba7d3f715153a64391470`.
  Public serial install E2E passed with all 983,023 bytes acknowledged and
  cleanup zero. Two independent boots each passed all 15 cases, with complete
  and qualified true, confirmation zero and no failed/blocked/not-run cases.
- Crypto image SHA-256: `7c552f713fb8ed700cb3b88f6e145bbbeefbc57e3036a929dfc95ced270d73b9`.
  Managed install passed; device qualification failed: 19 passed, two ChaCha
  cases failed and AEAD capacity was blocked. The SDK disables ChaCha by default;
  Mosaico lacked the two enables already present on DevKit/AMOLED. Board defaults
  now enable ChaCha20 and ChaCha20-Poly1305. The rebuilt App passed all 22 cases
  on first boot and an independent managed reboot, both with confirmation zero.
  Public install E2E passed with all 934,129 bytes acknowledged and cleanup zero.
  App SHA-256: `f2d0cda58e44339fcd2f98d077085f4b1f693c9cfa15b24a6bb10646306cca91`;
  package SHA-256: `82681385c2806897e57c5c9fa3f6a527418e66202206088e313eb55647dac47f`.
  This App contains the board configuration fix on top of ae9a21d6. Final status
  retained the Loader above, selected App/P2, cleared Stage and reported result
  zero; the 65,536-byte coredump partition remained blank. Each boot has a full
  22-case ledger and a complete/qualified report; a truncated replay line at the
  USB reboot boundary is preserved in the raw log and is not counted as a case.

Loader self-update and rollback qualification remain pending. The Stage resume
above does not replace their public acceptance scenarios.

## Full-suite revalidation at 85487ab1

All 16 E2E packages build after the board Crypto configuration correction.
Hardware observations remain suite-specific; this is not a 16-suite PASS.

- Core: fresh managed install PASS and two independent boots each 41/41, with
  cleanup, clock restoration, confirmation and task-probe result zero. Image
  `66a1fbd0bffc7b4e56a1c78083d2765e3f0bb45a919488c4885eb5562cdbf24d`;
  package `36f507d3b7ca8bd3c63a51bdd08153ffe73e735336258368f4350095cff202f7`.
- JSON: fresh public managed install PASS and two independent boots each 15/15,
  with cleanup and confirmation zero and complete/qualified true. Image
  `3e0f22ae650361375d5c7e24e0fd88b657fc195dc9ba0eb7d15f1e1596d9210a`;
  package `00e52ca99c1b2ddf0205ba39bb37bdbbe032eb7217fe9cde1038164a586adc8d`.
- Storage: the repository verifier accepted all 36 cases over five independent
  boots in order 1/2/3/4/4, including persistence and repeated empty-state checks.
  The first host install runner exhausted its eight reconnect attempts before
  the stress phase confirmed the App; its FAIL receipt is preserved. The App
  completed successfully, and a separate reinstall of the same immutable package
  passed the public managed-install E2E with a longer host reconnect budget.
  Image `ef2ebb1ec7efd5db8db21d177ceba9b5b65be81589de82a24fa1bad20b6070b2`;
  package `ac0a045baba6ad63f2437839c7cd527d5c611795c57f89606d1b2a1e8b989a33`.
- Audio Decoder: the shared verifier accepted 29/29 on two independent boots,
  each decoding 169 frames / 505,856 PCM bytes, with retained=0 and confirmation
  zero. Public managed installation passed. Image
  `75d1ad9dc59aa5913c71d9828dce4b1e370d816037d1cf40149362f657db364c`;
  package `7ca6c8fd0cf4214b67e80fc598aabead4d807c4901c317689bb0ceac5c7b5e60`.

Each completed revalidation above checked UID, exact package/image, unchanged
Loader/P1, App/P2, empty Stage, result zero and unchanged blank coredump. The earlier
raw serial logs and binary artifacts were held in a temporary directory outside
Git; that directory was no longer available when this run resumed. The hashes
and observed results above remain historical artifact-bound records. Retain new
receipts and immutable packages outside Git in durable local storage. Lua Link requires a
real BLE peer; single-board startup cannot qualify its link sessions.

## Qualification continuation

The first Atomic execution completed the resource comparison with cleanup zero,
then failed at the redundant BLE command startup (`server_open=-3`); confirmation
was not attempted and Stage was not cleared. App image
`13fd31e7f3a14e7705cfdf5855eb09461c2a487d86d2faeaa154b37b1a6ef8d2`;
package `7ffbf8b7e162642aa3f2c28c5d28436aadc97a0eaa37fb166d1664cada9d793e`.
This is FAIL, not an admitted 56-case PASS. Atomic, libco and legacy Pref now use
the already-started serial command service with explicit UART capability and do
not require BLE to confirm unrelated suites. Lua Link keeps its real BLE startup.

Board defaults previously omitted NimBLE and the certificate-generation/DTLS-SRTP
features required by the shared ESP BLE and DTLS providers. Enable the existing
provider requirements at the board SDK boundary, matching the reference boards;
do not replace their assertions with unsupported results. These settings require
new builds and device revalidation. No suite is relabeled as passing on these new
artifacts from an older receipt. Lua Link still needs a real opposite-role board,
and network suites need reachable fixtures and independent peer witnesses.

The first unified `mosaico-e2e-20261010-r2` Display App executed 8 PASS / 1 FAIL /
15 NOT_RUN: `rgb888` returned UNSUPPORTED. Its entry incorrectly selected the
reference AMOLED's conversion/clipping profile. Mosaico's actual provider accepts
only RGB565 and rejects partial bounds. The Mosaico artifact now passes its
fixed profile directly to the existing portable App; shared device helpers and
reference entries remain byte-identical. Mosaico selects RGB565/no-clipping and
still executes all 24 cases, including exact unsupported-format and unchanged
output assertions. No mandatory case or cleanup gate is removed. This first run
is FAIL, with confirmation not attempted; the corrected artifact needs a rerun.
Two separately identified Mosaico boards are available for the shared Wi-Fi fixture
and opposite-role Lua Link. Their new artifact entries reuse the portable runners;
the Lua join build compiles the same startup source as host with a fixed peer role.

Atomic rerun (`mosaico-e2e-20261010-r2`): public managed installation passed and
both managed boot and independent ordinary App reboot each admitted 56/56 cases,
20/20 workers joined, cleanup zero and confirmation zero. Execution IDs were
`dd9fdb1ba2ae1afcda3f3d0e31a4d4f8` and
`1c5e3385ed3b2540e0dd88986de1501e`. Loader/P1 was unchanged,
Stage cleared, final result was zero and the coredump remained unchanged blank.
App SHA-256: `7e085cdaa4694845052a510e2e84eed9e1aee6dc54e13cf4ffebddbecb5f2938`;
package SHA-256: `c8cc1363600f3f8c062a75c2e513b24d4db6d5b016a1f5d964e30f850aa4c134`. This receipt belongs to the
recorded build inputs, not to subsequent artifact changes. All 19 packages
(16 suites, two peer artifacts and Loader) built after BLE/DTLS configuration;
Display's profile correction is separately built as r3. Overall qualification
remains pending.

Display rerun (`mosaico-display-20261010-r3`): public installation passed; both
managed and independent ordinary App boots passed all 24 cases with cleanup zero,
confirmation zero and 23 completed-DMA output observations each. RGB565 and
no-clipping were fixed before execution; unsupported optional-format cases still
checked exact errors and unchanged output. Loader/P1, empty Stage, result zero
and unchanged blank coredump passed. The user separately confirmed normal
four-quadrant output and the brightness sweep on this artifact. This human
observation is recorded separately; the DMA report still has optical_verified=0.
App SHA-256:
`2268ef0b099d2a15963713abe5d07a02bd5dda72aa88c876d74a30c5af7ffc71`;
package SHA-256: `cd6fc350bd443e506885e4eb75dd1f97f94e621ec540257acfeb6aa8e4059c23`.
libco's Mosaico entry now uses an affine CPU0 64 KiB stack and a bounded initial
USB observation delay; its coroutine stacks and 10,000-switch contract are
unchanged. The revised package builds; device evidence remains pending.

Audio r2 public installation passed with all 1,232,331 package bytes acknowledged,
cleanup zero and empty Stage. Actual device output reported 24/24, 941 microphone
frames with nonzero peak/energy, 943 speaker frames and 30,014 ms soak; confirmation
was zero. However, the diagnostic interface reconnected after the early BOOT and
first cases, so the strict independent-boot oracle did not admit this run. Preserve
the incomplete first capture instead of inferring a fresh run from replay. The
Mosaico entry now delays initial startup three seconds for CDC enumeration and
explicitly flushes its BOOT line. Rebuild/retest is required; this is not a full
Audio two-boot qualification or an acoustic-pressure measurement.

Audio r3 rerun (`mosaico-audio-20261010-r3`): public install passed and both
managed/independent ordinary App boots had complete BOOT markers and 24/24
qualified ledgers. Both reported nonzero actual microphone peak/energy and output,
soak >=30,000 ms, cleanup/confirmation success and unchanged Loader/P1, empty Stage,
result zero and unchanged blank coredump. Acoustic observation remains separate.
App SHA-256: `dc08247d9c44b5a4ba446dca3d6bcbd0d26d6492c14146055131c4e199d32c45`;
package SHA-256: `2315367f2b9ec2e24f551fd2d8af894fa377796dae02f2f2f432127508397364`.

libco r3 failed: all 1,223,085 package bytes were acknowledged, but the App reset
with SDK reason 4 (PANIC) before a complete phase ledger or confirmation. Public
install E2E returned timeout (-7). The coredump partition remained blank, so no
backtrace is claimed. The r4 diagnostic entry uses an internal CPU0-affine native
stack, precise startup markers and artifact-only flash coredump configuration.
It preserves the same 8 KiB coroutine stacks, six phases and 10,000 switches;
this placement change is a diagnostic hypothesis, not a proven root-cause fix.
Core/Crypto now gate confirmation on complete success and Core's probe/clock
restoration; Lua confirms only after all five real peer rounds pass. These
Mosaico-only safeguards build and still require their own physical reruns.

## Stack protection, dual-board identity and recovery continuation

The r4 libco diagnostic still panicked after its first stackful switch. A real
20,384-byte flash coredump was saved and decoded with the matching ELF. The SDK
substitutes a synthetic frame when the current SP is outside its recorded task
bounds, so its synthetic PC/SP must not be reported as a real crashing address.
The hardware stack monitor and FreeRTOS interrupt return use the TCB stack range;
changing SP alone leaves them describing the root stack.

The RV32 backend now has private weak platform hooks around its actual assembly
SP change. The S31 SDK adapter captures/restores the task's active stack bounds,
updates the SDK TCB accounting and stack watchpoint, and holds a critical section
across the SP transition. Hardware monitoring remains enabled. The platform hook
is an independent native component selected by Mosaico composition, with no
changes to shared PAL task APIs or the existing ESP task provider sources. Other
RV32 builds retain no-op hooks; Xtensa and desktop backends are unchanged. The
adapter depends on the pinned SDK layout and must be revalidated on SDK upgrades.

`mosaico-libco-20261010-r5` passed all six shared phases and 10,000 actual switches
on both managed and independent ordinary App boots. Public installation passed;
Loader/P1, empty Stage and result zero checks passed. The pre-existing real
coredump was downloaded before and after and remained byte-identical, SHA-256
`67118d17d7a99844923a433271058656e1bde17be4d197ddc842cf88bc7aa66c`.
App SHA-256: `9b73bdaa23fc9d8b3ca310725c23115790cb25bf52d4d17d2edfe6193d0d5d65`;
package SHA-256: `fcfd363f421b25c0a15e599d91815c1793adc5af5ab11ba32fa1a7330cbb77fd`.
This pass used the adapter before it was separated into its optional component;
the final component composition requires another device run.

Core `mosaico-e2e-20261010-r5` passed all 41 cases on two independent boots with
probe/clock restoration/cleanup/confirmation zero, public managed installation,
unchanged Loader/P1, empty Stage and the same preserved real coredump. App
SHA-256: `f3ecb0e0a3585b2c783259168b7a6d3d7c608bbb65aed72921ad26ebef2bef7b`;
package SHA-256: `db2db9ab29ec28266e99660947d9181f6690674c1011937e2367965cb897be1d`.
It does not qualify later USB/composition changes.

Both 16 MB Flash images were backed up before writes. The second board's ROM MAC
is `1c2904d09548`, public UID `1c2904d09549`; the DUT remains `1c2904d0a629`.
Second-board Loader ROM flash and public Type-C identity/status passed. Simultaneous
Lua host/join testing exposed default USB serial `123456` collisions: macOS moved
port names between devices during reset, and the first capture selected channels
by filename order. The host emitted five successful rounds, but missing reliable
peer/startup/managed-install evidence prevents qualification. These interrupted
attempts are retained as not admitted, not a dual-board PASS.

The dual-CDC artifact defaults now set an empty TinyUSB serial string. Pinned
esp_tinyusb 2.4.0 derives its persistent serial from the eFuse base MAC in this
case. Both Loader and App select the same defaults. Host capture identifies
physical USB location and actual data-interface number (1 for CDC0, 3 for CDC1),
then checks the protocol UID before a mutation; lexicographic device names are
not authoritative identities. Unique enumeration and managed reconnection need
physical reruns. Pref adds a bounded startup delay and version marker so its
seed/verify/clean boots are independently observable.

A subsequent DUT `reboot loader` acknowledged the command, but the host reported
an incomplete transition and the new diagnostic boot still ran the Lua App.
Second-board management also timed out twice. This is a recovery failure under
investigation, not a successful App-to-Loader return. ROM refresh of the public
Loader is planned with existing dumps preserved; ROM repair does not replace the
public self-update/return/rollback gates. Network suites have not executed yet.

The historical shared PAL Wi-Fi consistency audit currently rejects changed
`.bazelrc` inputs from the S31 integration. Its recorded hashes were not rewritten
and its old board evidence was not reused. New Mosaico qualification remains
separate; this historical audit is not reported as passing.

## Final coroutine memory layout correction

After the adapter was isolated, `mosaico-e2e-20261010-r7` panicked during the
later libco phases. Its real 20,640-byte coredump and matching ELF were saved;
this failure supersedes the earlier two-boot confidence and does not erase the
r5 artifact-bound observations. Pinned FreeRTOS also locates its FPU/PIE/HWLOOP
save-area bookkeeping from the active TCB stack top. The first adapter changed
that top without reserving or initializing this metadata, allowing interrupt
saves to interpret ordinary coroutine bytes as task state.

A private stack-preparation hook now reserves SDK metadata above the coroutine's
usable SP. The S31 switch hook allocates the supported coprocessor save buffers
in the original task stack before replacing its bounds and propagates their
bookkeeping to the destination stack. Buffers remain owned by the RTOS task;
coroutine closure cannot free them. TCB bounds, watchpoint and hardware protection
still follow the actual SP. Default hooks on other RV32 builds remain no-ops.
The real-adapter host tests exercise metadata reservation, task-owned buffers,
interrupt saves, nested switches and root restoration with guards on and off.

`mosaico-e2e-20261010-r8` then passed all six phases and 10,000 switches on each
of ten independent boots. Public managed installation, exact App/package identity,
unchanged Loader/P1, empty Stage and final result zero passed. The new real
coredump remained byte-identical before/after, SHA-256
`1a4b21392369112099de8e553d68874fd9ffde7c872c15258e6e99c65ed9704a`.
App SHA-256: `0394afca0e9121d791005188c5862436b10db2cb22fc05538a0f609de6cb64ed`;
package SHA-256: `814015f7f362ae844bbb879fd81d807c6902ddf9621dfc4c78c5d670e3086c53`.
All non-document source inputs, including new files, were frozen with this build
and still matched at completion. Other suites need the final r8 package reruns.

Both boards now enumerate with distinct eFuse serials after Loader-only refresh.
Their current ROM partition tables were read and verified before writing only
OTA0 and otadata. NVS, App/P2, filesystems and coredump were not flashed.
The refreshed Loaders and public UID/status handshake succeeded on both boards.
Peer JSON r7 15/15, Crypto r7 22/22, Audio Decoder r7 29/29 and Audio r7 24/24 each
passed on two independently captured boots, including public managed installation,
cleanup/confirmation and Loader/Stage/coredump checks. These remain tied to their
r7 input snapshot; the later coroutine correction is not silently attributed to
them. The Decoder captures were validated directly after a host receipt-source
consistency assertion detected the ongoing source change; no device ledger was
regenerated or repaired from another boot.

## Build and execute

Prepare the pinned S31 SDK per the board README, then build an individual package:

```sh
bazel build --config=esp32s31 //projects/e2e/targets/h2loader_tar_zlib/pal-core/esp_mosaico:package
```

For direct-flash investigation, preserve the partition roles: `h2loader` / OTA0
is at `0x20000`, while the E2E App / OTA1 is at `0x220000`. SDK-generated
App factory flash arguments place its binary in the first OTA slot; using those
arguments unmodified for a managed App causes the command preflight to reject
the recovery slot with `H2_PAL_ERR_INVALID_STATE`. Install through a qualified
Loader for acceptance. A diagnostic ROM flash into the App slot with explicit
OTA selection is useful for debugging but does not prove managed installation
or the OTA pending/confirm/rollback sequence.

Change the suite segment for the entries above. Build success is a separate field
from execution. After device/transport/fixture validation, follow the corresponding
shared suite's device procedure and collect the actual first execution, immutable
package binding, independent restart, resource cleanup and peer witness receipts.
For MQTT, the host verifier accepts only the exact `esp_mosaico` / `esp32s31` pair
and enforces the same ESP provider cleanup and confirmation contract as DevKit.

Do not mark PR #709 ready or close Issue #708 based on this file alone. Record real
results and exact artifacts after execution; keep unavailable hardware/fixtures
visible as blockers. Never populate evidence with the reference board's receipts.

## Artifact-bound continuation status (2026-10-10)

Eleven of the sixteen portable suites now have admitted device receipts. The ten
r8 suites in the table were built against the frozen non-document inputs of
`a304f25e`; their startup/library inputs are unchanged by the later observation
fixes. HTTP r9 includes its own USB observation delay and flush change. Its two
independent boots each passed the entire 45-case registry, with different run
nonces and independent server arrival/TLS-rejection receipts. Both public Loader
r7-to-r8 self-updates and subsequent independent Loader reboots passed; this
does not qualify the still-pending deterministic rollback/checksum matrix.

Both boards retain their verified full-flash backups and immutable package/native
ELF/map archives outside Git. The real DUT coredump was preserved byte-for-byte
through these completed suites. ROM repair of the 8 KiB OTA selection restored
the Loader and resumed the previously staged HTTP r9 image; that repair and
resume are separate from the subsequent successful public HTTP installation.

The Lua r10 entry records read-only running/next partition and OTA states around
the existing shared reboot path and flushes diagnostic logs. It does not change
OTA selection, confirmation policy or the portable BLE test. MQTT, Net/TLS,
Wi-Fi, WebRTC and Lua Link admission, deterministic Loader rollback/recovery and
final peripheral observations remain required. PR #709 remains draft.

## Type-C and bilateral diagnostics continuation

The Windows USB production-fragment test passed after moving compilation to the
Bazel host toolchain. Native platform CI then found an unscoped Python generator;
its generator and source targets now use the existing host compatibility set.
Host callback tests and S31 board analysis pass.

MQTT's entry incorrectly selected the native UART sink despite the Type-C-only
connection. A separate optional board CDC0 output component now supplies bounded
writes and checked drain. Only MQTT opts in; command CDC1, Runtime UART, shared
PAL and reference-board providers are unchanged. Two channel/fault/ledger host
tests pass and the r15 package/native artifacts build and are archived. Hardware
MQTT admission remains pending.

Both r11 and r14 Lua roles produced five successful rounds and observed final
hold connections. Their operator windows expired before the requested physical
reset, so those attempts do not qualify physical loss. Confirmed App-to-Loader
return still failed even after the link became idle: selection was Loader/NEW
before reboot, followed by Loader/ABORTED and fallback to the confirmed App. The
existing DUT 20,640-byte coredump stayed byte-identical.

An independent Loader debug package records startup milestones in the reserved
`mos_ota_diag` test namespace through an internal-stack SDK safe call; it never
erases NVS or changes normal Loader confirmation. Lua diagnostic entries query
actual OTA states and initialize the SDK NVS reader without formatting. SDK query
errors remain visible. The r18 control/experiment packages have the same fifteen-
minute operator window. The experimental package additionally invokes the
existing PAL BLE stop API before shared reset; it is not a default platform fix
and its effectiveness is unqualified.

The shared E2E App adds optional `hold_timeout_ms`, with zero preserving the
five-minute default on existing boards. Transfer-suite deadlines and the Lua
script are unchanged. Fake-air tests verify transfer timing ignores the hold
override, default/configured hold succeeds, and deadline expiry releases all
owned resources.

The actual r18 host completed five rounds, confirmed the App and observed
`lost:-10` 4,039 ms after the last datagram following the user's second-board
reset. The second board's USB interface was absent and its r18 identity and full
boot ledger could not be read. Retain this as partial host evidence, not complete
bilateral qualification. The required MQTT, Net/TLS, Wi-Fi, WebRTC, full Lua
admission and Loader recovery/rollback gates remain open.

## S31 reboot preparation and r19 continuation

The r18 controlled peer experiment called the existing BLE stop API through the
internal-stack worker and reported `safe=0 stop=0`. Its subsequent public return
started the Loader with a new persisted startup counter (3 to 4). The normal DUT
control returned to App instead. This comparison supports full local-controller
shutdown before S31 restart; it does not establish an exact upstream SDK defect
or qualify the incomplete bilateral Lua run.

The preparation now belongs to the native ESP power provider, after its optional
observation hook and before SDK restart. It calls the existing native BLE stop
provider through a safe internal-stack context only for S31 with local NimBLE.
Other chips, disabled BLE and hosted BLE remain no-op profiles. Worker and stop
errors propagate; the helper does not extend PAL or change OTA confirmation.
Temporary application-owned shutdown variants were removed. Four actual-helper
host profiles pass in both normal and optimized builds; assertions are retained
under `NDEBUG`. Further real BLE return cycles are required for admission.

All nineteen r19 packages build and are archived with their native ELF, map and
Flash files against frozen source inputs. The subsequent assertion-only test
source delta is recorded separately; no archived build hash was rewritten.
The peer passed public Loader self-update and an independent Loader reboot,
including a real P1 package checksum and unchanged blank coredump. The DUT Core
candidate is verified in Stage with its running P1/P2 unchanged; ROM-assisted
startup recovery is still required and is not counted as public installation.

The r19 peer Core package passed all 41 cases on managed installation and an
independent App reboot. Clock restoration, the task probe and App confirmation
passed; P1, empty Stage and blank coredump were preserved. This does not replace
the remaining active-BLE return and bilateral Lua gates.

The native power review also corrected next-boot restoration after preparation
failure: skipping an SDK write requires both running and selected partitions to
match the target. Merely running the target is insufficient after a different
slot was selected. The four optimized profiles cover same-slot preservation and
both restoration directions; no direct otadata manipulation was added.

The r20 Loader, Core and normal lifecycle fixture build and have immutable
package/native bindings. On the peer, the confirmed lifecycle App starts the real
BLE command service and returns through the public protocol to Loader on three
successive warm cycles. P1/P2 image and package checksums remain unchanged,
Stage remains empty and last_result remains zero. The fixture's managed install
also preserved the blank coredump. This is a BLE-enabled command-service return
check, not a bilateral Lua transfer or physical-disconnection admission.

## Common IDF 6.2 and October 11 qualification

The user expanded this change to unify all GizOS ESP SDK consumers. S3/P4/C5
now select the same `ddb3852ab5c7f9e6dbc268203ef9d612de391bf8` SDK commit and
GCC 16.1 baseline as S31. CI/release install that exact checkout and all four
compiler targets, then restore explicit SDK aliases after the legacy provider
export. Both project-level IDF component constraints select the 6.2 series.
P4 remote 1.6.5 recognizes the SDK's integrated Wi-Fi implementation; Hosted
stays at its existing version. This is a pinned SDK revision, not an unbounded
SDK branch update.

IDF 6.2 requires explicit PSRAM component discovery in the native PAL composition.
The task provider rejects unavailable external stacks when the Kconfig option
is absent. The existing private SDK coprocessor adapter is now selected by the
RISC-V task provider: S31/P4 retain task-owned FPU/PIE/HWLOOP buffers, and targets
without coprocessors retain libco defaults. Xtensa gets no RISC-V override.
Builder regressions, task policy and missing-option compilation, hardware-guard
profiles, zero-coprocessor behavior and optimized assertion checks pass. Actual
S3 AMOLED Lua/Wi-Fi/Loader and reference Core builds, P4 reference and board
Display/remote-Wi-Fi/PAL builds, C5 reference and S31 MQTT builds pass. These
results do not claim physical P4/C5 execution or the complete CI graph.

The DUT r20 Core passes 41/41 on two independent boots and its public r20 Loader
self-update/reboot passes with the original 20,640-byte coredump retained. The
r20 Net/TLS core profile passes on two independent boots: 37 mandatory cases,
38 total passes and one explicitly unsupported optional case, covering 21 core
operations. Actual fixture records show typed TLS rejection, no handshake or
payload for invalid configuration/CA, successful recovery, cleanup and App
confirmation. Optional full-Net capability is not qualified by this profile.

MQTT r20 passed 36 functions but failed raw global allocation equality; that
failure and unconfirmed status are preserved. The r21 artifact separately tracks
actual borrowed management allocations with synchronized native usable sizes.
All allocations still reach the native counters; raw, command-owned and
App-owned totals are reported separately. Only command-owned live buffers are
excluded from App-owned cleanup. The actual-helper regression includes a leak
that would otherwise cancel command-buffer frees. Both r21 boots pass 36/36,
provider release, delivery-before-confirmation and exact broker/TLS witnesses.
App-owned before/after counts are 18/58,796 bytes, and provider release removes
one actual 32-byte allocation. P1, empty Stage and the original coredump are
preserved. The host verifier accepts Loader progress only before App execution;
late progress/startup, replay and missing confirmation still fail admission.

The operator-connected AMOLED is identified through the public protocol and has
a full 16 MB pre-test backup (SHA-256
`4bb1c08604e8d7c852afbbe0b4608d1800f74881832bd5e043594c4b3baac3cd`).
Its public 6.2 Loader self-update and independent Loader reboot passed before
the later Lua return failure. Both Mosaico r22 host and AMOLED r3 join completed
five actual rounds (RTT, reliable bursts, datagrams and 64 KiB streams), confirmed
their Apps and reached the final connected hold. The peer's public Loader reboot
was accepted but management did not recover. Mosaico observed actual lost:-10
4,100 ms after the last datagram, then reported extended-advertising removal
rc=15 / Loader adv_restart=-9. These partial records do not admit complete Lua.
Subsequent ROM probes received no serial response. After the user's reset/power
attempts and host USB serial control-line release, actual SPI boot logs and public
Loader management recovered without a Flash write. P1/P2 checksums remain valid,
Stage is empty, the peer coredump is blank, and the DUT original dump is unchanged.
The DUT public Loader return passed. This assisted peer recovery does not turn
the earlier timed-out public reboot into a PASS.

WebRTC r22 passes all 43 cases on two independent boots with distinct run IDs,
ordered first-execution ledgers, zero retained allocations, cleanup and App
confirmation. Actual Pion transport covers encrypted data and Opus; the wrong
fingerprint returns typed TLS_VERIFY (-17) with authentication evidence. This
does not establish physical microphone/speaker quality. The immutable receipt
binds clean source `2268ed71`, the r22 package/image hashes and unchanged original
20,640-byte coredump. Both boots retain Loader identity and empty Stage.

Artifact-bound records now cover 15 of 16 portable suites. Historical receipts
retain their original source and image identities. Complete bilateral Lua,
final peripherals/camera insertion/acoustic observations and Loader recovery,
rollback and checksum gates remain open. Earlier receipts are not relabeled as
final-source qualification. All nineteen r22 packages build against frozen
inputs; subsequent source changes require separate binding and regression.

## Wi-Fi version identity failure

The first r22 seed ran 38 mandatory functions with zero failures/blocks and
restored Settings/network, cleared the private backup and retained no resources.
Persistence correctly remained PENDING (-9) and the App was not confirmed (-7).
However, the 34-byte requested version was truncated by IDF to the 31-byte
runtime descriptor, while the package retained the full version. The public
status exposes both values and retained Stage; this image is not admitted.
One independent fixture lease record was also lost during host monitor reconnect;
no missing witness is reconstructed or treated as passed.

The ESP native runner now rejects empty, NUL-containing, invalid UTF-8 or over
31-byte application versions before SDK/environment/output work. The actual SDK
CMake cuts PROJECT_VER to 31 bytes; rejecting truncation preserves package/runtime
identity on S3/P4/C5/S31. All 36 runner regressions pass, including ASCII and
multibyte byte boundaries and fail-before-build checks on all four targets.
Other SDK version contracts are unchanged. The corrected short-version Wi-Fi
image still requires real independent boots, fresh peer witnesses and restoration.

## Wi-Fi r23 and delayed BLE completion

The short-version r23 package is bound to clean source `9c5867bf`. Public serial
install E2E passes with all 1,238,051 bytes acknowledged, cleanup zero and complete
true; it uses the existing configurable 1,000 reconnect attempts. The earlier
CLI's fixed retry count ended before the four-minute App confirmation and stays
recorded as a failed host operation. It is not substituted for the passing E2E.

Managed boot 3 and independent normal boot 4 each pass all 39 ordered cases,
covering all 21 operations. Nonces are distinct; both read the original canonical
Settings across a real boot, restore Settings/network, remove the backup, retain
no resources and confirm the App. Each fresh 600-second AMOLED fixture window
records six ordered real DHCP client leases across WPA2/open/hidden modes, then
restores its own Settings/network and confirms. Both receipts preserve Loader,
empty Stage and the original 20,640-byte coredump; the peer coredump stays blank.
The rejected truncated-version seed and its missing peer marker remain separate.

The r22 Lua log reports default-set ADV_COMPLETE at 72,633 ms and CONNECT at
72,738 ms, while the Loader restart grace is 100 ms. The native callback closed
extended advertising at both events. An actual-callback host regression proves
that a restart between them is cleared by the delayed CONNECT. The native fix
leaves extended completion with ADV_COMPLETE and retains CONNECT completion for
non-extended legacy advertising. Default and independent-set restart cases,
ordinary completion, central-role/failure handling and legacy behavior pass;
assertions remain active with explicit NDEBUG and optimized builds. This shared
ESP provider change requires no public PAL API or per-board configuration change.
Fresh S31/S3 builds and real bilateral loss/return are still required; host tests
do not yet qualify the new callback artifact or resolve the assisted peer return.

## S3 peer warm restart control and pending physical reset

The clean `9fbdd654` Mosaico r24 host and experimental AMOLED r5 join each
complete five real rounds and confirm their Apps. The candidate extends the
existing internal-stack, full local BLE stop before restart from S31 to S3;
other targets, disabled BLE and Hosted remain no-op profiles. All seven profiles
pass optimized host tests, including stop/worker failure propagation and restoring
App selection after a failed preparation. It changes no public PAL contract.

AMOLED r4 lost USB management after warm launch despite functional peer traffic;
the user's true power cycle restored confirmed App/P2 and a blank coredump.
This does not establish a particular SDK defect. The r5 candidate passes public
Loader self-update, independent Loader reboot and bilateral five-round control.
The public peer Loader return produces actual lost:-10 without advertising errors;
both public Loader returns pass, Stage clears, the original DUT dump is unchanged
and the peer dump stays blank. The experimental source diff is bound separately
(SHA-256 `e0f18f004dd7b6f0e294561917214cae9de8bedbd999b985988363d561d8dbe6`).

A subsequent operator-reset window expired without physical loss. The peer's
five-minute default then closed gracefully; neither timeout nor peer_closed is
admitted as physical loss. The AMOLED launcher now selects the same existing
15-minute hold option as Mosaico, leaving the portable default, transfer deadlines
and assertions unchanged. New artifacts must pass a fresh operator reset, actual
lost, post-loss advertising and both public Loader returns before Lua admission.
Historical r24/r5 control receipts are not relabeled as final-source qualification.

## Clean r25 continuation and Pref fixture identity

Against frozen clean `d2646cf1` inputs, r25 passes public managed installation
and independent startup qualification for Core 41/41, JSON 15/15, Crypto 22/22,
Storage 36/36 over five boots (1/2/3/4/4), Decoder 29/29, Atomic 56/56 with all
workers joined, and libco's six phases / 10,000 switches over ten boots. Every
receipt retains Loader identity, cleared Stage and the original 20,640-byte dump.
These seven receipts retain their source/artifact identities through later changes.
Fresh r26 HTTP/MQTT and r27 Net/TLS/WebRTC fixtures and packages are prepared;
building them does not establish device qualification.

Both clean r25 host and r6 AMOLED join pass five real rounds and confirmation.
A controlled public peer restart produces actual lost:-10 without advertising
errors; both exact Loader returns and unchanged dumps pass. The separate hardware
reset attempt is not admitted: USB reenumerated while the peer retained its old
runtime tick count and no lost event occurred. The operator confirmed an attached
battery and BOOT/PWR-only controls. PWR shutdown and later startup restored the
correct Loader with a blank dump, but happened after the connected control ended.
A fresh connected physical power-off/lost/power-on remains required for that gate.

The r25 Pref install found the previous COMPLETE phase and ran only its single
empty-state assertion, so it cannot close the 31-operation seed/verify/clean run.
It did not confirm this newly installed image; the cancelled host wait and the
one-case device ledger remain failed/incomplete evidence.

The Mosaico Pref artifact now records its linked ELF identity in the portable
suite's control namespace. A new image clears only h2e2eprefctl/h2e2eprefdata,
commits the data cleanup before publishing the identity, and then runs the
unchanged shared suite. Ordinary boots of the same image preserve all phases,
including the final empty recheck. Malformed markers and I/O/close errors fail
closed, release resources and prevent suite execution/confirmation. Device
configuration, the native Pref provider and reference launchers are unchanged.
A real SQLite host regression verifies all four persisted boots, unrelated
configuration preservation, migration from COMPLETE, invalid markers and ten
failure paths with optimized NDEBUG assertions. The corrected image still needs
real managed install, complete 31 operations and its independent empty recheck.

## Visible E2E phases and fresh October 12 evidence

The Mosaico Loader and all 16 suite launchers now compose the synchronous
project-level `mosaico-screen` presenter; Mosaico Wi-Fi peer and Lua join artifacts
also include it. Display runs first. As soon as board configuration supplies the
existing display PAL, artifacts show STARTING, suite identity/number and current
phase at 100% brightness. Loader callbacks show waiting/installing/launching;
Pref distinguishes persistence reboot, Audio labels the real 30-second output,
and Lua distinguishes five transfer rounds from its peer-power-cycle wait. Lua
session completion is not an overall qualification PASS.

One serialized artifact caller renders a static RGB565 stripe with the existing
bitmap font. It adds no worker/task and initializes before suite resource
baselines. Display releases presentation ownership while its original 24-case
registry owns the pixels, then restores the result page. All presentation return
codes have separate diagnostics. Host admission must check those codes together
with actual case/cleanup/confirmation ledgers. Pixels and sound still need human
observation. The green page requires both the actual suite gate and firmware
confirmation; failed Storage runs now remain unconfirmed instead of taking the
previous unconditional-confirm path. No shared PAL contract, portable case
registry, board-owned application task or reference launcher changes.

Optimized NDEBUG tests pass for rendered bounds/status pixels, input/geometry,
draw/present/brightness errors, failed-close ownership retention and reopen after
Display suspension. The existing production Audio failure/confirmation recovery
oracle also passes for Mosaico, AMOLED and BK7258. Experimental r31 Loader public
self-update/independent restart and Display 24/24 on two boots pass. The operator
confirmed visible STARTING, PATTERN AND DIMMING and the green PASS page; its
separate observation receipt is tied to the actual r31 source/package/image.
r31 is an explicitly dirty source cut, not a clean-head or overall-pass claim.
Reviewed r32 builds all 19 native targets successfully and has its own frozen
inputs/configuration and immutable artifacts.

The pre-presentation October 12 runs retain their original identities: r28
Pref executes all 31 operations and a separate empty recheck, with new-image
fixture reset and same-image persistence; r28 Display passes 24/24 on two boots
with operator-confirmed pattern and dimming; r28 Audio passes 24/24 on two boots
with actual microphone/output and at least 30 seconds of output. The operator
was not watching that Audio run, so acoustic proof remains pending. Frozen r26
HTTP 45/45 and MQTT 36/36, and r27 Net/TLS core profile (37 mandatory/38 total
passes, one optional unsupported) and WebRTC 43/43 each pass two real boots with
fresh server/TLS/Pion witnesses and actual resource/cleanup/confirmation gates.
These receipts preserve the original Loader/Stage/coredump checks. Their source
cuts are not relabelled after presentation changes.

Overall status remains NOT QUALIFIED. The screen changes require fresh
image-bound suite qualification, paired Wi-Fi and connected physical Lua loss,
Loader recovery/checksum/crash matrices, final board/camera insertion and actual
acoustic observation. Builds and historical or partial receipts do not close
those gates; PR #709 stays draft.

## Current integrated-code portable qualification (October 12)

All 16 portable suite profiles now have admitted fresh device receipts for the
code committed as `22bc43f7`. The r32 frozen dirty input record retains its
original identity and a separate committed-source equivalence record: every code
file matches that commit, with only the two qualification documents differing.
r33/r35/r36 use clean committed inputs and exact immutable artifact/configuration
hashes. Source cuts and observations are never relabelled. The table above lists
the current software-profile evidence; older investigations remain historical.
Net/TLS qualification is its mandatory core profile, not optional full-Net.

A host restart stopped Pref installation and destroyed temporary tooling/server
keys. All archives and complete receipts survived. Persistent tooling and the
exact SDK/submodules were restored. Missing first Pref execution was not invented:
a distinct archived r28 image prepared only the two private Pref namespaces,
then the exact r32 archive reran all 31 operations and the independent empty 1.
New actual HTTP/MQTT/NetTLS/Pion fixtures required separately rebuilt r35/r36
network artifacts, which pass their original two-boot and peer/resource gates.

The first paired Wi-Fi controller incorrectly used upgrade monitor while the
trial fixture retains Stage until its 600-second confirmation. The CLI's bounded
verification exited -7 and the DUT installer was cancelled; the software 39 ledger
and discontinuous peer capture remain NOT ADMITTED. After actual peer trial
completion/confirmation, fresh ordinary-App monitor windows qualify managed and
independent-normal r33 runs with six complete leases and final restoration each.
No CLI verification, test assertion or confirmation gate was weakened.

The r32/r6 Lua pair executes five actual rounds each, verifies both immutable App
identities and confirms connected hold. The operator's PWR4s-off/5s-off/PWR2s-on
cycle yields actual lost:-10 after 4100ms with 1766 received ticks. Both subsequent
public Loader returns have exact P1 identities, empty Stage/result 0, unchanged
original 20,640-byte DUT dump and blank peer dump. The earlier PWR recovery before
hold is separately retained and does not stand in for this connected observation.

Overall status remains NOT QUALIFIED and PR #709 remains draft. Public Loader
lifecycle/recovery/checksum/crash/URL/Wi-Fi scenarios are now being executed, with
the actual original dump verified and backed up before destructive crash tests.
Current acoustic observation and final board/camera insertion/physical peripheral
observations still block complete acceptance. A green software result does not
close those gates or establish SOC/magnetic calibration or live camera removal.

## Loader control App internal-memory correction

The r34 public recovery sequence exposed three real failures in the Mosaico
control App: Wi-Fi scan, Wi-Fi connect and URL staging returned -4. The Loader
counterparts passed. Device logs show that Wi-Fi could not allocate its six
internal static RX buffers after the control App had started BLE and reserved
a 64 KiB internal entry stack. The controlled partial run is retained as
NOT ADMITTED; passing individual checksum transitions do not admit the full run.

Only the Mosaico H2Loader E2E control artifact now places its entry stack in
PSRAM, matching the Loader and suite launchers. Existing internal safe-call
stacks still own flash and OTA confirmation. Startup diagnostics record actual
free/largest internal 8-bit heap before and after command/BLE startup. No shared
PAL provider, portable registry or other board launcher changes are required.

Frozen r39 inputs explicitly include this one-file diff on ec0b919a; all five
normal/alternate/crash/Loader artifacts build and are archived with hashes.
On the real device, entry internal free/largest are 228239/131072 bytes and
after command startup 127647/63488 bytes. App Wi-Fi scan, connect and URL
staging now each pass through the unchanged public runner. Full lifecycle,
crash/rollback and both checksum-format matrices are still running and are not
qualified by those three passes. Original coredump bytes were verified and
backed up externally before the intentional crash/erase cases; preservation
of that external backup does not imply the device dump remains unchanged.

## Expanded workload entries and portable logging correction

The requested scope now includes Mosaico iperf, WebRTC performance and GizClaw
business entries, in addition to the 16 portable suite profiles, board diagnostics
and complete Loader recovery. Their native graphs reuse the existing portable
Apps. Each artifact owns a PSRAM entry, dual CDC and serialized screen stages;
the inventory is 19 positions. No shared PAL API or reference-board behavior
changes. These new entries have no admitted device results yet.

The current cross-target CI found newlib standard-stream ABI dependencies in
the project stage presenter. It now receives a borrowed Log PAL explicitly from
all Mosaico artifact callers and uses bounded formatted messages. Panel failures
retain priority; absent/failed diagnostic sinks cannot return successful admission.
Optimized host tests cover real records, sink failures, overflow, nineteen suite
positions and display ownership. Existing device receipts retain their original
source cuts; this presentation change requires separately bound regression.

iperf retains the ordered TCP/UDP/SCTP client matrix and real server; client
confirmation follows workload success and SCTP cleanup. Server mode is a serving
fixture, not a matrix PASS. WebRTC performance runs one unchanged shared smoke
workload with real offer/STUN peers and observed metrics; failed attempts remain
unconfirmed. GizClaw retains the full shared business registry, exact execution
ledger, runtime profile, zero retained resources, cleanup and confirmation. Its
prerecorded PCM voice profile is software-only (physical_audio=0); requesting
physical audio explicitly fails as unsupported. Microphone/speaker acceptance
still belongs to the separate board/Audio gate. No AEC binary ABI is qualified.

The GizClaw service/registration identity, profile, known AppConfig bytes, HTTPS
objects and NTP are explicit private fixture inputs, matching the reference
boards. Empty fixtures fail before business mutations while preserving the
public recovery channel. No production identity or secret is provided by the
source default. Real configured peer and backend evidence remains required;
a build or unit test is not business, throughput or hardware qualification.
Overall status remains NOT QUALIFIED and PR #709 remains draft.

The first expanded r41 build correctly rejected GizClaw as incompatible: both
its library and portable App dependency selectors lacked S31 and fell back to
the desktop SDK. The selectors now route only S31 through the existing firmware
SDK core and Opus headers, exactly as S3/P4 already do. Other platform branches
are unchanged. The rejected build remains failed evidence; a fresh artifact cut
is required. This is build composition, not a new public API or S31 business PASS.

## Verified expanded build and public recovery

The expanded r42 source cut builds all 22 native targets: 19 suite entries,
Mosaico Lua join and Wi-Fi peer, and Loader. All source inputs and immutable
package/native files were verified before any later document edit. The cut
retains its explicit diff on 38a2c156. Five optimized host targets pass for screen
logs/errors/rendering, iperf, WebRTC performance, GizClaw launcher state and the
ABI guard; three shared GizClaw App/firmware-link/header regressions pass. The
actual C5 GCC/nm firmware archive check also passes after the Log PAL correction.
These results are build/host evidence; the first 16 rows retain their earlier
22bc43f7 device bindings, and the new entries do not inherit those receipts.

Separately, the unchanged public r39 recovery runner completes 51/51 cases on
the directed Type-C CDC1 IO Stream iKCP channel. Its v2 JSON report SHA-256 is
`89a05d9750d26ab8c6f1252cfa1a803fdc63fd04fa75dc40c67647370c7b1853`.
Actual coverage includes Loader and App commands, Wi-Fi and URL staging, abort,
Stage-preserving App/Loader restarts, Loader self-update, five within-format
transitions for each format, all eight cross-format transitions, intentional
pre-confirmation crash/rollback, and coredump read/dump/erase checks. The dump
read is 20,928 bytes; subsequent erase is actually blank. The test-preserved
crash Stage was then aborted through the public command with Loader/P2 image
identity unchanged. The externally backed-up original 20,640-byte dump still
has its recorded hash; the intentionally replaced/erased device dump is distinct.
Optional non-frame UART log monitor and desktop BLE transport are not claimed
by this selected CDC1 report. The r39 receipt remains bound to its original cut
and the separate code-equivalence record for 38a2c156.

Overall remains NOT QUALIFIED. Fresh Log PAL/presentation device regression,
real performance and configured business runs, current acoustic observation and
final board/camera insertion/physical peripheral gates still require evidence.
PR #709 remains draft.

## Current physical sound failure and performance reset

The latest r42 Loader self-update/independent boot, Display 24/24 on two boots
and native Log PAL stages pass. The operator separately confirmed current
stages, quadrants, dimming and green PASS on that exact Display artifact.
Audio also passes 24/24 on both r42 boots, but the operator reports no sound
during the second executing LISTEN window. This is physical audio FAIL; it is
not replaced by API success or historical audible beeps.

The unchanged shared suite sets speaker volume to 40 percent and creates the
stability track at 300 milli. Its small PCM fixture may be quiet; the root cause
is not established. The Mosaico artifact now labels the 30-second phase I/O soak
and supplies an operator-controlled BOOT102 three-beep comparison after software
completion. It uses 100 percent speaker/unity track gain and a bounded 4096-peak
600/1000/1400 Hz fixture, verifies effective volume, then drains, closes, stops
and restores volume. Cleanup failure enters existing recovery. Hearing remains
explicitly unconfirmed on the screen and in records; no shared Audio provider,
portable registry or reference launcher changed. Optimized cue waveform and
eleven injected failure paths pass, together with all three actual board Audio
failure/confirmation recovery regressions.

The real configured r43 iperf candidate executes the first six TCP/UDP transmit
cases twice, then returns to Loader while entering UDP receive. Remaining cases
and the complete 14-case matrix are NOT ADMITTED. The actual candidate/Stage/P2
identities are retained; current dump is blank and reset cause is unknown. The
first managed installer was stopped after final-state waits; the separate public
upgrade ACK was accepted while its short confirmation verification failed.
These operations are not converted into passing managed installs. The private
oracle now matches the shared wire contract's lowercase `result=pass`; no
firmware assertion is relaxed.

Frozen r44 builds the Audio cue and both performance diagnostic packages against
exact source/configuration hashes. Performance-only defaults enable bounded
panic evidence in the reserved managed coredump partition, matching the existing
libco diagnostic pattern. All three native builds pass; artifacts are archived
before later document changes. This is not a device or reset-cause diagnosis.
The loud comparison and actual diagnostic workload reruns remain pending, as
do valid controlled GizClaw service inputs and remaining final hardware gates.
Overall remains NOT QUALIFIED and PR #709 remains draft.

## Current manual audible cue observation

The operator pressed BOOT on the actual r44 Audio App and heard all three
100-percent/unity-gain beeps. The executing cue records work/close/stop/restore
all zero, saved and effective speaker volume 100, and 60 PCM blocks. A separate
receipt binds that observation to the exact UID, package/image hashes and raw
executing segment. The unchanged 24-case suite, at least 30-second I/O soak,
cleanup, confirmation and screen stages pass on both r44 boots.

This verifies basic bounded audible speaker output on this artifact. It does not
relabel the earlier r42 low-gain no-sound observation, establish calibrated
acoustic quality, microphone accuracy or AEC, or prove that the shared low-gain
stability waveform was audible. Remaining source/performance/business/peripheral
gates keep overall status NOT QUALIFIED.

## Actual performance comparison and reference business audit

The complete r44 and r45 iperf runs each report six passes out of fourteen.
All four UDP receive and all four SCTP cases time out with -6. Cleanup succeeds,
confirmation is refused and the executing screen reports FAIL. Exact candidate,
Stage/image identities and blank coredump status are retained. r45 enables the
SDK Wi-Fi/LwIP PSRAM-first policy only in the two performance artifacts. Internal
minimum free memory improves from zero to 31 KiB, but the complete matrix still
fails; the allocation flag alone is not a verified functional fix.

The same real repository server/client completes all fourteen cases on host
loopback with the original five-second durations. This isolates a useful host
control, not a Mosaico pass or a proof of root cause. Actual r45 WebRTC performance
completes DTLS/SCTP connection but its data-only download stops at 49,152 bytes
with a callback error; the full 10 MiB/scheduling workload is not admitted.
The failure leaves an unconfirmed App and the public Loader return succeeds. The
current dump remains blank. No panic cause is inferred.

Frozen r46 builds and archives both performance artifacts with the additional
reference small-malloc threshold zero. DMA capability requirements, radio
advertising, workloads, time budgets, throughput/fairness gates and confirmation
remain unchanged. Native builds pass; real device comparison is separate and
pending. These settings change no shared board default or provider contract.

AMOLED/BK business qualification uses the same shared GizClaw App and explicit
private fixture flags; its historical controlled Dev endpoint/profile and eight
case catalog are documented in the Mosaico entry. Local HTTP/Pion and fault
injection do not replace actual registered business services. No compatible
private registration/AppConfig/media/NTP fixture was found in the current test
environment. Mosaico now has an explicit `esp_mosaico` choice in the shared API
auditor. All eighteen auditor regressions pass, including matching complete
Mosaico evidence and rejection of foreign-board, truncated and failed runs.
Current full acceptance requires 228 APIs, not the historical 227-API receipts.
Overall remains NOT QUALIFIED and PR #709 remains draft.

## Latest complete failed workload records

r46 WebRTC performance reaches 278,528 download bytes and times out at the
unchanged 300-second transfer deadline. Its callback error flag is zero; that
is still workload FAIL, with confirmation refused, blank dump and a successful
public return to Loader. r47 adds eligible external static BSS placement: iperf
completes 11/14, internal minimum is 59 KiB and DMA minimum 27 KiB. The remaining
failures are `udp_rx_40m` and `udp_rx_20m_1200` (-6), and `sctp_tx_1400` (-4).
Cleanup is zero and confirmation remains refused. r47 WebRTC receives 3,964,928
bytes before the same 300-second deadline; it also remains FAIL and returns to
Loader publicly. All original candidate, source/configuration and failed ledgers
remain separately bound. These observations do not establish a driver or network
root cause, or complete performance qualification.

The real host WebRTC smoke completes its original transfers, three distinct
request channels, loaded/data-only throughput ratio 1113 permille, audio gap
limits and 200 echoed media frames, with actual process exit zero. This is a host
control, not a Mosaico receipt. Frozen r48 builds and archives a Mosaico WebRTC
artifact with transport worker priorities 20/21 and lwIP 22, matching the verified
AMOLED business scheduler hierarchy. No shared provider, other board policy,
workload, fairness/audio limit or confirmation gate changes; real device results
are pending. The existing AMOLED iperf server role also builds against pinned
IDF 6.2. Its package is publicly staged with exact P1/P2 and coredump preservation,
but is not activated or counted as a client matrix PASS.

The shared API auditor's Mosaico extension also passes its optimized Bazel test.
Real GizClaw business remains blocked on the reference private fixture inputs;
all eight cases and the current 228 APIs still require actual complete execution.
Overall remains NOT QUALIFIED and PR #709 remains draft.

## PAL loss-marker correction and self-created business fixture

The exact r49 artifact completes two full real WebRTC smoke boots: loaded audio
p99 is 137.895/110.597 ms, loaded/data-only ratios 1148/930 permille, and deadline
and would-block counts are zero. Both full transfers, distinct request channels,
resource/confirmation/screens, peer witnesses, preserved P1/dump and public
Loader return pass. This receipt retains its original frozen source identity.
Final diagnostic placement r50 passes its first boot but fails the independent
second with a callback error; existing confirmation metadata cannot qualify that
fresh failure. r52 identifies the exact branch: zero-length Opus envelope.

The PAL event contract explicitly defines zero-length OPUS_FRAME as one loss
marker, and the H2Peer RTP decoder emits NULL/0 for erasure. The shared performance
App now handles that marker without inventing a received frame, sequence bit,
timestamp or latency sample. Final missing-frame budgets (smoke five, benchmark
zero), duplicates/reorder, audio gap/producer deadlines, throughput/fairness and
cleanup remain unchanged. Actual callback regression runs with assertions enabled
under optimization; valid markers leave receipt/timing unchanged, missing
sequence bits stay absent, and malformed nonempty data/future timestamps fail.
Both focused host targets and native Mosaico/AMOLED builds pass. The first r53
hardware attempt still fails the unchanged 300-second data-only deadline: its
10 MiB download takes 298.177 seconds and only 573,440 upload bytes complete.
All failed source/package/ledger identities remain distinct. A complete final
performance qualification is still pending.

The operator authorized self-creation of business fixtures. An isolated official
GizClaw 0.24.1 container is now running at the exact published image/source digest;
new server/admin identities, RuntimeProfile, random registered token and known
AppConfig are created and read back through the real Admin CLI. Private HTTPS
objects pass actual chain/name/date and bytes/SHA checks, and the separate NTP
fixture passes origin/system-clock checks. The initial opaque firmware-version
resource is rejected; a newly built genuine `1.0.0-mosaico-e2e` Core package is
then registered with its actual hash/size and readback, leaving the old object
unchanged. Configs, keys, tokens, artifacts and logs remain outside Git.

A project-owned optional CA provider consumes the existing explicit fixture flag,
keeps REQUIRED verification, preserves the BSP provider for empty input and owns
its borrowed API throughout the App's command-recovery lifetime. Allocation and
provider failures preserve the old API and release temporary bytes; Runtime init
failure destroys the unused owner. Its optimized ownership/failure tests and
actual configured native GizClaw/Core builds pass. This is not a shared PAL API
or other-board trust change. Real ASR/TTS/workflow resources and actual complete
eight-case/228-API device business execution remain pending. Overall remains
NOT QUALIFIED and PR stays draft.


## DevKit peer replacement and shared SCTP regression fixes (2026-10-13)

The replacement peer is an actual ESP32-S3 DevKit, MAC `30:ed:a0:a9:57:ec`,
16 MiB Flash and 8 MiB embedded PSRAM. Its complete original factory Flash
backup is retained outside Git with SHA-256
`2d3887c6dd9a903401c8dc02ee91d43fe6b953ee9043755865ad0b2e53709148`.
The DevKit-specific public Loader was rebuilt on pinned IDF 6.2, flashed and
read back; its managed iperf server was installed through the public protocol.
No AMOLED artifact or execution identity is attributed to this new board.
Mosaico was independently backed up in full, then only its 8 KiB OTA selection
was restored with verified readback; firmware, configuration and crash
partitions were preserved.

The source-bound `1.0.0-mosaico-e2e` Core package installs publicly and completes
41/41 on both managed and independent boots. Task/stack probe, cleanup,
confirmation, unchanged P1/coredump and public return to Loader pass. The first
private observer used an incorrect summary prefix and timed out; that failed
observer record remains separate from the subsequent complete actual-ledger
verification. Replay is the firmware's immutable ledger, not reconstructed
results. Other portable profiles still require final-source regression.

With the actual DevKit peer, frozen r54 iperf completes 13/14: all UDP cases
and `sctp_tx_1400` pass, but `sctp_tx_1200` fails. Diagnostic r55 completes
12/14; both SCTP uploads fail. Its actual first send backpressure is -9
(`WOULD_BLOCK`), followed by the harness's 1001-attempt limit being converted
to -4. The 1200-byte case exhausts those retries in roughly 184 ms. The shared
iperf emitter now preserves the PAL/SCTP transient-result contract, with the
unchanged caller send/run/handshake deadlines providing the bounds; permanent
errors and partial datagram writes fail immediately. One numeric diagnostic
per association is retained. The production callback regression verifies
2000 busy returns, recovery, saturated diagnostics and immediate fatal errors.

Frozen r54 WebRTC performance receives more than 5 MiB before its unchanged
300-second transfer timeout. A separate legal-input regression reproduces
a shared SCTP zero-window deadlock: retained higher gap TSNs fill the receive
buffer and prevent the lower retransmission from closing the gap. The provider
now reclaims only a sufficient suffix of higher, undelivered, nonassembled
gap fragments in linear passes, preserving cumulative acknowledgements and
assembly. Dropped-gap SACK state and advertised space are updated, including
a following allocation failure. A fresh SACK remains scheduled while older
output is busy; the service retry then advertises the current gap/window state.
Ordinary DATA, I-DATA, TSN wrap, rejection of still-higher data, allocation
failure and busy output are covered in sixteen regression combinations. The original failing
records and the successful 15-target host check remain outside Git.
This portable provider change does not alter a PAL ABI, board task policy,
workload, loss/latency/fairness threshold or confirmation gate. Final native
Mosaico and DevKit build/device qualification is still pending.

An unconfirmed rolled-back candidate is intentionally not automatically
relaunched while Stage still describes the same image; the shared Loader
regression explicitly protects that state. Earlier same-image installer waits
are not proof of a board-driver failure or successful installation. Recovery
uses a distinct verified package and retains the old failure evidence.
Overall remains **NOT QUALIFIED** until the remaining final-source, physical,
performance and real GizClaw business gates are actually completed.


## Shared protocol and cancellation continuation (2026-10-13)

Frozen R58/R12 native packages build on pinned IDF 6.2 and execute on the
Mosaico DUT and independently identified ESP32-S3 DevKit. The actual unchanged
iperf registry completes 12/14: all four SCTP send/receive cases now pass, while
`udp_rx_40m` and `udp_rx_20m_1200` time out on the control connection. The peer
records the corresponding closed sessions; no incomplete result is admitted.
R58 WebRTC fails at connection setup with a DataChannel closed/error event,
before a transfer ledger, and its ordinary public return to Loader succeeds.
These failures do not qualify the performance workload or establish an
unverified driver or network root cause. Failure-only client state/server phase
witnesses are added without altering the protocol, load, deadlines or gates.
R59/R13 native builds and immutable source/package/image archives pass;
two-boot device execution is in progress.

The host serial E2E CLI now uses its existing cooperative cancellation hook
for SIGINT/SIGTERM, allowing ordinary protocol/session cleanup to run. Its
optional handshake timeout is separate from the command timeout; defaults and
the initial status bootstrap remain unchanged. Twenty optimized related host
tests pass, including actual iperf3 interoperability, portable SCTP regressions,
Loader App tests and real signal delivery. On the Mosaico, cancellation after
393,216 acknowledged bytes returns a cancelled/failed operation with cleanup=0.
The following public status and blank-coredump check succeed; P1 and P2 image
checksums are unchanged. The partial Stage is invalidated, as required for an
incomplete transfer. An earlier private observer incorrectly expected the old
Stage to survive an interrupted overwrite; its failed assertion is retained
separately from the corrected check of actual device state. Cancellation is
not installation or candidate qualification.

Complete compatible `//...` build graphs for ESP32-P4 and ESP32-C5 pass on the
pinned SDK. The ESP32-S3 graph was interrupted to prioritize the requested
physical E2E continuation; its full-graph result remains pending, while the
DevKit-specific R13 build passes. Build results establish compile/link/ABI
compatibility for the selected graph, not runtime GPIO, radio, audio or power
qualification. Remote ESP CI results and physical P4/C5 regressions are not
claimed. The original worktree and unrelated serial devices are untouched.

The owned official GizClaw server and NTP fixture have been restored. Strict
HTTPS verification exposed missing X.509 authority identifiers; regenerated
certificates retain the existing keys/subjects and add the required extensions.
Strict chain/name/date verification, object hashes and NTP pass. Previous
private configuration/certificate snapshots are preserved outside Git; the new
CA requires a separately bound business firmware build. Legitimate ASR/TTS and
workflow catalog services, actual eight-case/228-API business execution and the
remaining final-source peripheral/profile gates remain pending.
Overall remains **NOT QUALIFIED**.


### R59 complete iperf qualification

The frozen R59 Mosaico client and R13 DevKit server complete the unchanged
fourteen-case matrix on both the managed and independent restart: 14/14 twice.
All 28 actual peer records match the ordered protocol/direction cases. Screen
STARTING/PASS, cleanup=0, confirmation=0, package/image identity, Loader P1,
Stage cleanup, blank coredump and ordinary public return to Loader are checked.
The actual report SHA-256 is `a01fa39d78a6d895e2cdefa8a6e717915a641aaba8dd650a2d07e447bcf8eefa` and its immutable source/configuration
binding is retained outside Git. Functional input hashes match the committed
protocol/cancellation implementation; only qualification documentation changed
after the native archive was frozen.

Earlier UDP/control failures remain retained. This completed run does not prove
an unmeasured network root cause, a throughput level beyond the measured report,
or completion of WebRTC performance and GizClaw business. Overall remains
**NOT QUALIFIED** while those and the remaining physical/profile gates are open.


### R59 WebRTC failure and R60 diagnostics

R59 establishes the actual DTLS/SCTP connection and receives 622,592 bytes,
then times out at the original 300-second transfer deadline. The old Pion peer
records a 60-second backpressure timeout with 132,484 buffered bytes. Candidate
qualification and confirmation fail; cooperative cancellation reports cleanup=0
and the following public management status/return to Loader succeed. The
source-bound executing and peer logs remain separate from the admitted iperf
receipt. Connection success is not WebRTC performance qualification.

R60 native build/archive and the updated Pion fixture build/protocol tests pass.
The existing board memory checkpoints additionally report the largest contiguous
internal/DMA blocks; total free space alone cannot establish whether a buffer
allocation can succeed. The peer's original backpressure timeout additionally
reports actual queued bytes, elapsed time, cwnd/rwnd, MTU and RTT. These bounded
diagnostics change no wire protocol, time/throughput threshold, load, allocator,
PAL API or worker policy. The new actual run is pending; no root cause or
passing performance result is inferred from the diagnostics.


### Reordered-message assembly lifetime regression

An independent legal receive sequence begins a message in the in-order assembly
buffer, receives its ending fragment out of order, then fills the middle gap.
Completion uses the copy path; the original implementation leaves the assembly
begin pointer referencing a fragment that delivery subsequently prunes. The
original optimized regression fails on that retained owner. Successful delivery
now releases the matching partial assembly metadata before pruning; WOULD_BLOCK
retains ownership for the ordinary service retry. Sixteen DATA/I-DATA, TSN-wrap,
blocked-callback and ordered/unordered combinations check exact payloads, owner
release and the next message; all eighteen related host targets pass. This
proven lifecycle defect is not asserted as an earlier board failure's root cause.
Native and device qualification for this additional shared repair is pending.

The separately frozen R60 WebRTC image completes its first full real smoke
iteration with the original transfer/request/audio/fairness gates and
cleanup/confirmation. Its independent second boot remains in progress; one boot
does not complete the agreed qualification or qualify the newer provider fix.


### R60 independent failure and current-source native checks

R60's independent second boot completes both 10 MiB directions, the request
batch and fairness (1064 permille). Its audio-only p99 interval is 222.004 ms,
exceeding the unchanged 200 ms gate; loaded p99 is 348.176 ms with five missing
frames. The actual result is FAIL, so the two-boot candidate is NOT QUALIFIED.
The ordinary public return to Loader succeeds. The accepted activation's
180-second CLI observer timeout is retained separately from the actual executing
first-PASS/second-FAIL ledgers; reattaching read existing capture bytes without
constructing results.

R61 Mosaico/R14 DevKit native packages containing the assembly-lifetime repair
build and are archived against frozen inputs. The latest protocol libraries
cross-build on both P4 and C5; their earlier complete compatible graph results
retain their own source identity. Current full S3 graph validation is running.
R14 package staging and upgrade acceptance are observed, but the upgrade
observer and one independent management probe time out. Its running version,
confirmation and crash state are not inferred. A normal DevKit hardware reset
is requested before further device writes; Mosaico remains in Loader.


## R61 failures, R62 receive-copy comparison and SZP link repair

R61's actual iperf run completed 12/14: `udp_rx_10m` and `udp_rx_20m`
failed with control timeout `rc=-6`, `after_state=10`, `data_done=0`; the
real DevKit had entered its data phase. WebRTC completed transfer/request
work but failed loaded audio scheduling (p99 485.791 ms, 95 frames and five
missing). Audio-only p99 was 185.646 ms and fairness 1273 permille. Both
Apps returned through the ordinary public Loader path. These are failures,
not qualifying receipts.

R62 immutable iperf/WebRTC packages build successfully against HEAD
`60a06a99` plus the recorded artifact-only receive-copy configuration.
`CONFIG_LWIP_L2_TO_L3_COPY=y` promptly releases fixed radio RX buffers;
the comparison retains all original gates. Its actual first iperf boot
has already reproduced UDP reverse failures, so this configuration cannot
be credited with eliminating the control starvation. The complete run and
WebRTC comparison remain pending; no R62 qualification is asserted.

The current S3 full graph exposed a genuine SZP audio-system DRAM overflow
of 64 bytes. The captured map places the 125,116-byte software Opus/PCM
scene in internal BSS. A SZP-only linker fragment now places that exact
scene in the board's enabled external PSRAM BSS. PAL microphone reads and
track writes copy their software buffers; driver DMA storage is untouched.
No buffer, stack or feature capacity is reduced. The focused SZP native
package build passes and its map verifies the entire scene in external
RAM. The original failure map is retained separately. The full S3 graph
and physical SZP compatibility are not established by this focused build.


### Completed R62 result and pre-start UDP regression

R62 completes 8/14 iperf cases: all four reverse UDP cases fail, plus
`sctp_tx_1200` and `sctp_rx_1200`. WebRTC completes both 10 MiB directions,
three request channels and fairness 1219 permille; loaded audio fails at
p99 300.364 ms with four deadline misses and fourteen WOULD_BLOCK results.
Both Apps return through the public Loader path. The receive-copy selector
is withdrawn; frozen R62 packages, failed observers and ledgers remain intact.

A desktop regression uses real loopback client/server sockets with a shared
receive-pool constraint: control cannot receive until an early reverse UDP
packet is consumed. The original client reproduces `rc=-6`, `after_state=10`,
`data_done=0` while the server reports closed in its data phase. A bounded
pre-start drain in duration-based reverse UDP now polls control and services
the existing peer-filtered stream without extending the control deadline.
The first measured sequence retains early-packet losses in ordinary UDP
accounting. Finite byte-budget runs retain their existing path so no finite
payload is discarded; this fix does not qualify shared-pool starvation in
that separate mode. The final regression/interoperability batch passes 5/5, and the additional
continuous-data/blocked-control regression verifies the original deadline and
cleanup. Fresh device qualification remains pending.

The S3 full graph at `837d6a60` retains frozen inputs but fails when the
Espressif component registry cannot be reached for the AMOLED BLE speed
client. No full S3 PASS is claimed. The focused SZP placement build and map
validation remain valid; this registry failure does not overturn that result.


### R63 independent-boot failures and network CPU fairness repair

R63 actual iperf completes 14/14 on the first boot and 8/14 on the independent
boot. The latter fails `udp_rx_20m` in pre-data control, followed by further
UDP/SCTP failures. The first boot is retained without granting two-boot
qualification. WebRTC first boot passes all original gates (audio-only p99
177.134 ms, loaded p99 114.204 ms, missing zero and fairness 1975 permille).
The independent boot completes baseline bidirectional 10 MiB but times out
during the original 300-second loaded transfer. Both public Loader returns
are verified, P1 remains unchanged and the final coredump is blank.

The network worker's per-round packet/message bounds do not bound the number
of consecutive busy rounds. A production-worker regression captures a public
command, sustains its processing under a deterministic CPU clock, and shows
no scheduling sleep across 80 ms in the original implementation. The repair
allows a 1 ms scheduling sleep after each 10 ms CPU slice, covering command,
receive and transmit continuations. Clock failure/backward movement also
yields; a failed sleep is a real terminal transport error and still cleans up.
Five control/event/channel/media/worker regressions pass, including all four
scheduler boundary modes. Every supported ESP board currently uses 1000 Hz
ticks, so the 1 ms delay blocks for a tick. This does not lower workload gates
or establish the cause of every physical failure.

The next iperf artifact adds at most one pre-start wait diagnostic per state:
control polls, actual early datagram/byte counts and elapsed time. This retains
the exact control deadline and loss accounting; it will distinguish remaining
physical timeouts from the independently repaired shared-pool regression.
Fresh R64 native/device qualification remains pending.


### R64 evidence interruption and native control-socket comparison

R64 actual iperf completes 12/14: `udp_rx_10m` times out before data and
`sctp_tx_1400` reports IO. At the UDP timeout the client has serviced 1,528
early datagrams / 2,230,880 bytes and polled control 29,545 times under the
original 30,000 ms deadline. This does not support unread client UDP data
as the sole explanation for that remaining control failure.

The old chat/worktree was archived during R64 WebRTC observation, removing
the checkout and its earlier diagnostic readers. The owned observer is
cooperatively cancelled; its cleanup and ordinary public Loader return are
retained. The run is incomplete, not a fabricated workload failure or PASS.
The current chat restores clean commit `91667398` into its managed worktree,
verifies all 5,400 frozen source hashes, and retains the original runtime,
backups and evidence. The previous chat stays archived. DevKit R14 identity,
Stage=0 and blank crash record are independently read back before restarting
current-chat-owned readers.

The pinned official iperf3 implementation enables TCP_NODELAY on its control
connections in both `iperf_client_api.c` and `iperf_server_api.c`. A native
POSIX/lwIP fixture view now does this only for the sequential control socket
of Mosaico and DevKit iperf entries; data socket options and the global Runtime
remain unchanged. Setting and actual nonzero readback must both succeed;
otherwise the owned socket is closed and IO is returned. Closing the control
session resets its identity for the next case. Real host sockets verify
client/default/source-bind and server accept paths, data defaults, descriptor
reuse, distinct-owner rejection, failed-option cleanup and retry. Both host
targets pass. No PAL ABI, capacity, workload or performance deadline changes.
R65 / DevKit R15 native and actual device validation remain pending.


## User pause checkpoint — 2026-10-13, R65 / DevKit R15

Functional source is `e548de105499376530316c6f021a1a8f1897c90e`; the
subsequent checkpoint commit changes documentation only. R65 and R15 native
packages are frozen against that source and pinned IDF 6.2. R15 managed
installation passes with cleanup=0; actual UID/image, preserved P1, cleared
Stage and blank coredump are verified.

**iperf qualified:** the unchanged fourteen-case matrix passes on two
independent actual Mosaico boots. All 28 ordered R15 peer witnesses match
protocol/direction and rc=0. Screen stages, cleanup, confirmation, source /
configuration / package / image / UID, P1/Stage/coredump and ordinary public
Loader return pass. Receipt: `perf65-20261013-1791883776197088000-iperf-qualification.json`.

**WebRTC not qualified:** first actual boot passes both 10 MiB directions,
three request channels and fairness 981 permille. Audio-only p99 is 184.652 ms
and loaded audio p99 is 177.139 ms, within the unchanged 200 ms limit. The
independent second boot is interrupted at the user's pause request; its last
retained download progress is 6,291,456 bytes / 212,721 ms. It has no complete
qualifying second ledger and is neither relabelled PASS nor a completed
performance failure. The first-boot receipt/log remains source-bound.

The owned controller, both serial readers and Pion server are stopped. Both
boards are no longer enumerated at pause, so a new public Loader-return and
coredump check cannot be completed after that disconnect. Reconnection must
read actual active role/version/images/Stage/crash state before any install
or restart; do not assume the App returned to Loader. Configuration, original
Flash backups, coredump evidence and immutable packages remain preserved.

Still pending: complete two-boot WebRTC qualification; current-source sixteen
portable profiles, board/peripheral/camera and full Loader recovery gates;
complete S3 graph; legitimate GizClaw eight-case/228-API execution; required
CI and fresh review. S3's focused SZP RAM repair passes, but its full graph has
no complete PASS. Historical P4/C5 full graphs and current protocol cross-builds
retain their own identities and do not prove physical compatibility. The real
isolated GizClaw 0.24.1 server currently lists zero models, voices and workflows.
No service key or business PASS is fabricated. Overall **NOT QUALIFIED** and
PR #709 remains draft. All original workloads and thresholds are retained.

## New-host continuation — 2026-10-13

The continuation starts from `f03b7ed91b8d240e30d2299951be3d86308c2fa2` on a
different macOS host. The operator explicitly defers GizClaw's complete eight
business classes because legitimate ASR/TTS/workflow services and injected
endpoint/registration configuration are unavailable. This is a scope exclusion,
not a business PASS. Existing saved Wi-Fi remains available.

Both actual devices have fresh, complete 16 MiB Flash backups on this host,
including partition SHA-256 inventories. Both captured coredump partitions are
blank. DevKit UID `30eda0a957ee` retains the R15 peer App and Loader
`0.2.3-atomic-541`; its public return to Loader and subsequent identity check
pass with empty Stage. Mosaico base MAC `1c2904d0a628` is read from the ROM;
its backed-up App descriptor is `mosaico-perf-20261013-r65`. ROM identity and a
Flash descriptor do not establish its current running role or confirmation.
After operator reconnection, dual CDC enumerates again and public status
confirms UID `1c2904d0a629`, the retained R65 App and R42 Loader. The public
serial status E2E passes. Returning to Loader and rereading status succeeds;
both original image checksums remain unchanged and Stage is empty.

Current host validation passes 326 distinct targets across board, portable App,
device-launcher, ESP native, network-worker, fixture/verifier and desktop E2E
tests. The initial Audio qualification check detects that Mosaico changed the
historically source-bound recovery test. Mosaico now selects the existing
`H2_TEST_BOARD_SOURCE` injection point in its build rule; the common test bytes
and original evidence hashes are restored. All three board recovery regressions
and the unchanged qualification evidence check pass. No historical receipt is
rewritten to claim a new device run.

The new host's real Pion smoke completes bidirectional 10 MiB, three request
channels and audio co-load: fairness 987 permille, loaded audio p99 27.748 ms,
100/100 frames and no missing, duplicate or reordered frames. A separate real
LAN run from this host to the retained DevKit R15 peer passes all fourteen
TCP/UDP/SCTP cases, with matching board-side records. These are host and DevKit
peer checks, not a fresh Mosaico qualification or a new two-boot receipt.

Local raw logs, backup inventories and `host-validation.json` are under
`/private/tmp/gizos-e2e-20261013/` on this host; Flash and credential-bearing
material stays outside Git. Pinned IDF/toolchain setup and fresh native/device
qualification are incomplete. GitHub DNS failures interrupted the toolchain
download; the official Espressif mirror supplies the same pinned archives.
After two automatic permission-review timeouts, the operator explicitly
authorizes toolchain downloads. The resumed download completes both pinned
GCC archives and verifies their official SHA-256 hashes. The SDK and recursive
submodules now match the pinned commits with a clean tracked checkout; official
tool and Python dependency checks pass. The BSP archive also matches the
repository SHA-256. Component-registry DNS fails through the host's default
resolver; a task-local Python fallback queries public DNS for the two official
Espressif component hosts while retaining HTTPS certificate verification.
Official `compote registry sync` processes prefetch the locked component
versions into a task-local partial mirror, allowing subsequent firmware builds
to reuse downloads. The Display, Core, Storage, Crypto, JSON, Atomic, libco,
Audio, Audio Decoder and Pref native packages build successfully on this host.

Fresh Display R66 (`mosaico-20261013-r66`) passes all 24 cases on two complete,
captured boots, confirms the App and returns through the public Loader command.
The operator confirms normal patterns, colors and dimming, ending with PASS.
Package SHA-256 is
`e4d516beb168db0f6cd574091cead505064b24ee2f49ed00a08eb4d52bbecc3a`;
image SHA-256 is
`427163d7b4ed6398596cd435abfcb36256dae1b4b385fe953d949f920956896e`.
R42 P1 remains unchanged, Stage is empty and coredump is blank.
Receipt: `display-r66-receipt.json` in the local evidence root.

JSON passes 15/15 and Crypto passes 22/22 on two independent actual boots each,
with successful managed installs, confirmation, empty coredumps, unchanged P1
and public Loader returns. Their immutable packages and `receipt.json` files
are in `json-r66/` and `crypto-r66/`. Remaining current-source device gates are
still in progress. Overall status remains **NOT QUALIFIED**.

Core R66 passes 41/41, Atomic R66 passes 56/56 and Audio Decoder R66 passes
29/29 on two actual boots each. Core retains the 100-cycle task-resource
probe; Atomic validates distinct execution identities and all worker joins;
Audio Decoder uses the shared ordered-ledger verifier. Each managed install,
App confirmation, unchanged Loader P1, empty Stage, blank coredump and public
Loader return is checked. Receipts are `core-r66/receipt.json`,
`atomic-r66/receipt.json` and `audio-decoder-r66/receipt.json` in the same local
evidence root.

Six R67 network packages build against the actual new-host LAN fixtures: HTTP,
MQTT, Net/TLS, PAL WebRTC, iperf and WebRTC performance. Native build success
does not yet qualify their device executions. The first Storage R66 managed
install transfers the whole package but its post-install reconnect times out;
subsequent authoritative status confirms the intended App, unchanged P1 and
empty Stage. Its phase-one replay reports 31 passes, but the first executing
capture is incomplete, so this attempt is not admitted. Its failure receipt
and later observations remain in `storage-r66/`.

The available DevKit now has an explicit `pal-wifi/devkit-fixture` target using
its own board graph and USB console, plus `lua-link/devkit-join` reusing the
DevKit launcher with a compile-time peer role. These provide the real second
board roles required for bilateral tests without selecting another board's
pin map. Their build and device results must be recorded independently.

libco R66 completes all six ordered stages and 10,000 context switches on ten
independent boots, with the expected CPU0 root and 8 KiB coroutine stack. Every
boot has its own captured startup, App identity and blank-coredump check; the
final public return reaches the unchanged Loader (`libco-smoke-r66/receipt.json`).
Audio R66 passes 24/24 on two boots with real microphone/playback frames and
30,007 ms / 30,018 ms stability intervals. Its API result is separate from the
pending operator three-beep observation (`audio-r66/receipt.json`).

Storage R68 (`mosaico-20261013-r68`) completes all 36 contract-2 cases over
phases 1/2/3/4/4, with the same version-bound nonce, real persistence, two empty
completion rechecks, unchanged P1, empty Stage and blank coredump. The managed
install passes in 131,099 ms with a larger host reconnect budget. A local
controller initially attempted to parse a still-incomplete JSON replay line;
the original execution capture and controller failure are preserved. Without
restarting the first boot, capture resumes at a fresh replay boundary and the
unchanged shared verifier admits a complete ordered replay, followed by four
independently commanded boots. The resumed receipt records that capture gap
explicitly (`storage-r68/receipt.json`); no device case is rerun to hide failure.

Pref R66 executes 17 seed operations, 11 persistence checks and 3 cleanup
operations across three actual automatically captured boots, followed by an
independent fourth boot with one empty-store recheck. The image-bound fixture
resets only on the first boot; confirmation, exact App identity, unchanged P1,
empty Stage, blank coredump and public Loader return pass
(`pref-r66/receipt.json`). Audio acoustic confirmation remains deferred while
the automated network suites run; its preserved package can be restored for
the manual three-beep check.

HTTP R67 passes 45/45 and MQTT R67 passes 36/36 on two independent boots each,
with exact per-run fixture arrival/TLS witnesses. MQTT validates post-delivery
confirmation and unchanged native resource counts. Net/TLS R67 passes its
original core profile on both boots: 37 mandatory / 38 total passes and one
optional unsupported case, zero retained sockets/resolvers/allocations. Each
boot has 39 finished same-boot peer records, including certificate rejections,
rejection-then-success recovery and twelve TLS churn sessions. The verifier CLI
has a Mosaico entry calling the unchanged parser; its existing three fresh-boot
rejection tests pass.
PAL WebRTC R67 passes 43/43 twice, including precise fingerprint rejection and
zero retained allocations, with separate Pion log slices. All four suites
confirm their exact images and return to unchanged R42 P1 with empty Stage and
blank coredump. Receipts are in `http-r67/`, `mqtt-r67/`, `net-tls-r67/` and
`webrtc-r67/`. Optional full-Net and complete device acceptance remain open.

iperf R67 fails its first actual boot: 13/14, with `sctp_tx_1400` returning
`-4` and zero receiver bytes. The failed install/serial/DevKit peer evidence,
unconfirmed App status and blank coredump are preserved in `iperf-r67/`.
Public Wi-Fi scanning subsequently finds the same saved SSID at about -36 dBm
on channel 11 while the connection used channel 1 at about -79 dBm. The saved
configuration has neither a BSSID restriction nor a channel hint. The ESP
provider previously left the SDK's first-match fast-scan default in place.
It now requests all-channel, signal-sorted selection for unpinned SSIDs while
preserving explicit BSSID restrictions and channel hints. A host regression
compiles the actual mapper and checks four pin/channel combinations; the
original mapper fails the regression. R70 network images and fresh device
receipts are required before attributing a throughput recovery to this fix.

The operator confirms Display R66 patterns, brightness changes and final PASS.
Audio R66 is restored for a second two-boot 24/24 run and the operator confirms
three clearly audible beeps (`audio-r66-audible/receipt.json`). That acoustic
observation is recorded separately from the API ledger; the supplementary
capture does not contain a `H2_PAL_AUDIO_CUE` line, so no cue return-code witness
is invented. The previous unanswered Audio receipt is retained unchanged.

At the operator's overnight handoff request, the non-GizClaw suite count is
14/18 with fresh device passes. iperf has an actual 13/14 failure; Wi-Fi,
Lua Link and WebRTC performance still need completed fresh acceptance. The
full Loader lifecycle/checksum matrices and final interactive board diagnostics
are additional gates outside those eighteen suites. Wi-Fi-related passes must
be revalidated on the final selection-policy fix. The earlier full S3 build
encountered component-registry failures, then was interrupted after the source
changed; it is not a successful final-source graph validation.

R70 WebRTC performance now passes the unchanged shared **smoke** contract on
two actual boots. The fixed driver selects the strong same-SSID AP at about
-35 dBm. Each boot transfers 10 MiB in each direction in both data-only and
loaded phases, uses three distinct request channels, and receives all 100
audio-only plus 100 loaded audio frames without loss, duplication or reorder.
Loaded fairness is 971/1017 permille (minimum 800); loaded audio p99 gaps are
92.275/88.669 ms (limit 200 ms). Upload is 637,644/640,975 B/s and download is
578,057/594,480 B/s. Package SHA-256 is
`779aba9c68eb1027413851c950b848522617f8920fa44615e0b3a762da510156`,
image SHA-256 is
`5a392c90e4565f8b8cbaf55ce2188351121499a90ce5595dac56d6c520d3a0a5`.
Pion peer records, exact image confirmation, empty Stage, blank coredump and
return to unchanged R42 Loader pass (`webrtc-performance-r70/receipt.json`).

The local controller initially misapplied the benchmark-only zero-backpressure
gate to this smoke run (first boot reports three WOULD_BLOCK observations).
The shared App and firmware were not changed. The controller failure and its
original source are preserved; the complete first execution is revalidated
against the actual smoke predicate before the second independent reboot.
This does not claim benchmark-profile acceptance.

The handoff therefore has **15/18 non-GizClaw suites with fresh device passes**;
iperf, Wi-Fi and Lua Link remain open, alongside the additional Loader/board
gates and final-source regression. Local backups, immutable packages, logs,
toolchain and `RESUME.json` are mirrored under the ignored directory
`.cache/e2e/20261013/`; credential files and Flash backups are mode 0600 and
must not be committed. A different LAN requires fresh fixture addresses,
certificates and corresponding network packages before continuing.

Final local checks pass: the three Wi-Fi mapper/settings/teardown tests and all
ten selected qualification/failure-recovery tests. The Net/TLS historical hash
test caught the temporary shared-CLI edit; that edit was reverted, and the
existing `pal-net-tls/esp_mosaico:verify_device` entry successfully validates
the real R67 first-boot ledger without changing historical source hashes.
R70's two performance packages, R71's eleven selected Mosaico build targets
and R17's four DevKit peer targets all build successfully against the pinned
SDK. Both devices finish on their original Loader images with empty Stage and
blank coredump; local LAN fixture services are stopped for the overnight handoff.


## 2026-10-14 current-host recovery checkpoint

The operator excludes GizClaw from acceptance. The original PR remains Draft.
Historical results retain their original source and image bindings; the newer
runs do not relabel them as current-head qualification.

The complete original public Loader recovery matrix now passes **51/51** on
`mosaico-recovery-20261014-r84`, bound to native source `a369658c` and the
preserved guard-equipped host runner. The actual execution takes 2,033,925 ms.
It covers Loader/App lifecycle and rollback, Wi-Fi and URL update/abort,
both package formats, the complete checksum rejection/cross-format cases,
crash-before-confirm recovery and coredump read/erase/blank verification.
The actual induced dump is 20,928 bytes, SHA-256
`adf6f3a153f81cb77e2d36f6349acf13e31c825a82d7db8a7d69e548f7e26a73`.
Raw protocol evidence and the original dump are retained before erase. Private
configuration, dumps, firmware and complete Flash backups stay outside Git.
The original full 16 MiB Flash restore is pending again after this matrix;
the earlier R77 restoration receipt does not describe the current device.

WebRTC performance remains **NOT QUALIFIED**. The unchanged smoke contract is
10 MiB each direction, three request channels, fairness at least 800 permille,
audio p99 at most 200 ms, at most five missing frames per 100, and no duplicate
or reordered frames. Zero backpressure/deadline observations are a benchmark
requirement, not an extra smoke gate. R85 passes fairness but fails loaded
p99; R86 and R87 also fail and their experimental RX/core settings are withdrawn.
The original board UDP mailbox is 32. R86/R87 explicitly used 6; earlier
notes mistaking the SDK default for the board baseline are corrected.

On the operator's available Wi-Fi, R88's independent native-policy preflight
receives 100/100 ICMP replies, p99 172.003 ms and maximum 188.702 ms. The actual
WebRTC audio-only phase nevertheless fails with 84 received frames and an
observed 2,556.87 ms sender gap; it never reaches the loaded phase. This is a
real device-side stall that independent network latency alone does not explain.
R89 is a normal host cancellation for missing diagnostic-code admission;
R90 is a native configure failure and was never flashed. Neither is a new
firmware performance failure. R91's corrected build admits the actual diagnostic
marker before installation. It completes the original workload but fails
p99 at 202.574/255.745 ms, with 100/96 frames and fairness 1173 permille.
R92 separates setup from connected operations: connected processing maxima
are command 1,912 us, media 508 us, packet pump 10,211 us, RTP send 978 us
and channel send 3,141 us. It still fails loaded p99 932.352 ms and receives
91 frames (nine missing), while fairness 853 passes. Its independent
native-policy ICMP p99 is 681.616 ms. Timings include preemption and exclude
time asleep/between operations; they do not prove all delay is external.
Both actual peers receive/echo all 200 frames; public Loader returns pass.

Opt-in worker diagnostics keep bounded operation wall-time aggregates and
emit them only after worker exit. They compile out by default. Four ordinary
host regressions and the explicitly enabled network-control test pass; the
source-bound R91 native build passes. These checks are not board acceptance.
The exact `a369658c` remote CI run `38508491577` finishes with a macOS arm64 UDP
fixture startup failure; other platform jobs pass. The focused local fixture
reproduction passes separately and does not relabel the remote failure.


A further scheduler defect is reproduced independently: the worker previously
started its next work budget from the timestamp taken before sleeping.
Late resumption therefore consumed the following budget and caused premature
repeated yields. The fix reads the resumed monotonic timestamp; failed clock
reads retain the prior fallback policy. A deterministic 25 ms rescheduling
case fails before the fix and passes afterward, alongside clock failure,
backward time, sleep failure and queued-response exit cases. The full 22
H2Peer tests and four focused regressions pass on this source. These are
host scheduling checks, not synthetic board performance measurements.

The functional R93 native image builds and passes admission with worker
profiling and preflight both compiled out. It retains the original 10 ms
work slice, packet/message limits, queue capacities, priorities, core/radio
policies, transfer sizes and all smoke predicates. Actual independent board
acceptance completes with FAIL: unloaded audio receives 100/100 frames with
p99 194.923 ms; loaded audio receives 93/100 with p99 319.993 ms. Fairness
1270 passes, sender gaps remain at most 30.460 ms, and deadline/backpressure
counters are zero. The independent peer receives and echoes all 200 frames;
board loss markers identify consecutive gaps of six and one frames. Both
ends use HAIVIVI-MFG according to the operator. These results do not yet
identify whether loss occurs on Wi-Fi, SDK receive queues or the decoder.
The public R84 Loader returns successfully. No WebRTC PASS or final Flash
restoration is claimed.

R96/R97 add bounded, default-off receive diagnostics. R96 receives/authenticates
188 RTP packets while the actual peer echoes 199: one fixture packet is absent
on the uplink and eleven return RTP sequence numbers never reach H2Peer.
No received packet fails authentication. R97 receives all 200 packets and
passes both audio predicates (100/100 frames, p99 105.846/132.089 ms), but fails
fairness at 733 permille. Link wrappers identify exactly one H2Peer UDP mailbox,
with 29,293 actual posts and 105 failed posts. Wrappers preserve SDK calls,
results and capacity; SDK sources remain pinned and unchanged. The original
R94 SDK-source probe is rejected at build admission and withdrawn. R95 does
not reach media because the host IP changed and its signal endpoint became
stale; R96 and later use the newly verified endpoint. Neither admission/setup
failure is presented as a performance measurement.

The byte-pressure fix moves the existing 16 KiB threshold from the shared UDP
receive pump to reliable message delivery. SCTP retains a blocked message for
retry, while RTP and transport acknowledgements continue to be received.
The 12-event pump limit, physical queue capacities, priorities and smoke gates
remain unchanged. A real event-queue regression first fails on the old code,
then verifies bounded reliable delivery, concurrent audio, exact payloads and
recovery after the consumer resumes. R98's single diagnostic boot passes all
original smoke predicates: audio 100/99 frames, p99 103.756/157.081 ms and
fairness 942 permille. It still records 149 failed UDP mailbox posts, so the
passing result does not establish that mailbox loss has been eliminated.
R98 is one diagnostic boot; a legacy controller log saying "two boots" is an
incorrect label and does not extend its evidence scope.

Sustained-consumer regressions additionally reproduce SCTP retransmission
being suppressed by blocked delivery, and a reopened zero receive window not
being announced. Timer service now continues transport work during consumer
backpressure and schedules a pending SACK when delivery reopens that window.
Both regressions fail before their respective fixes, then all 36 H2Peer/SCTP
recursive host tests pass. The final default-off image still requires two
independent real-board boots. Full original Flash restoration remains pending.

### R99 default-off failure and R100 receive timing (2026-10-14)

- Pushed functional repair head `eeecac6fe1fba376a937c64d5b07603ce1ec5f42`;
  all 36 H2Peer/SCTP host regressions pass. R99 is source-bound to this head,
  with all four optional diagnostics disabled and original smoke gates intact.
  Its first boot fails loaded audio p99: **206.243 ms > 200 ms**. Unloaded p99
  is 148.016 ms; both phases receive 100/100 frames with no duplication or
  reordering; fairness is 1315 per mille. The real peer receives/echoes all
  200 frames, with echo processing at most 0.108125 ms. No second-boot or
  final WebRTC qualification is claimed. Public R84 Loader return is verified.
- R100 adds bounded, default-off receive-arrival and post-decode timestamps.
  Three focused host tests and the native diagnostic build pass. One actual
  diagnostic boot fails: p99 97.548/573.422 ms, frames 100/88, fairness 890.
  All 188 recorded RTP packets authenticate; the longest loaded arrival gap
  is already 568.203 ms before decoding, while per-packet decode takes at most
  1.578 ms. Connected packet-pump operations are at most 9.502 ms. The actual
  peer receives/echoes 200 frames; SDK UDP mailbox statistics are 28,562 posts
  and 171 failed posts. These counters do not identify which packet types
  were dropped. R100 is diagnostic evidence, not acceptance.
- Both endpoints use HAIVIVI-MFG, as confirmed by the operator, but the host
  currently uses 2.4 GHz channel 6 and the DUT uses channel 11. Independent
  host-to-gateway ICMP during load receives 97/100 responses with p99
  1038.437 ms; after WebRTC traffic stops and public Loader return is verified,
  the idle comparison receives 100/100 with p99 49.004 ms. This supports
  investigating load-related network queuing; ICMP priority differs from
  media traffic and does not by itself assign the entire failure to Wi-Fi.
  The host BSSID is unavailable; the operator requests autonomous
  investigation. A private, RAM-only board AP selection enables a channel-6
  comparison without claiming the same AP. Host network settings are unchanged.
- Receipts: `rtc99-performance-result.json`,
  `rtc100-performance-diagnostic-result.json`,
  `rtc100-host-radio-observation.json`, and both
  `rtc100-host-gateway[-idle]-ping.json` files in the private 20261014 ledger.
  The original 16 MiB backup SHA and mode were reverified; historical first
  restoration receipts are preserved under
  `board77-first-restore-before-loader84/`. Final original Flash restoration
  remains pending. Loader R84 recovery remains source-bound **51/51 PASS**;
  GizClaw remains excluded and PR #709 remains Draft.

### R101 AP comparison and R102 yield timing (2026-10-14)

The board connects to an observed channel-6 AP selected from the public Loader
scan. The host remains on its original network; its AP identity is unknown.
R101 has all four diagnostics disabled and fails its first boot with loaded
p99 **203.927 ms > 200 ms**. Unloaded p99 is 99.474 ms, both phases receive
100/100 unique ordered frames, and fairness is 1047 per mille. This is still
not two-boot acceptance. The private AP selector checks the actual association
and never writes the Loader's saved configuration.

R102 adds actual cooperative-yield duration to existing default-off worker
timing. Two focused enabled host tests pass. Its one diagnostic boot passes
all original smoke predicates: unloaded/loaded p99 117.881/180.004 ms,
100/100 frames in both phases, fairness 984. Yield takes at most 1.423 ms;
connected pump/media/RTP/channel operations take at most 7.907/0.437/0.905/
1.425 ms. The SDK mailbox records 29,589 posts and zero failed posts. This
single instrumented pass does not replace the failed default-off result.
Both R101 and R102 return to the public R84 Loader with blank crash storage.

A default-off Pion `--rtp-dscp` option now supports a controlled direct-UDP
media-priority comparison. It keeps DTLS/SCTP at DSCP 0, serializes socket
marking with concurrent writes, checks OS TOS readback at startup, and exposes
actual write counters. Boundary/concurrency/error/real-socket tests pass in
Bazel and Go race mode; the new write path is fully covered. Marking does not
prove the AP honors QoS. R103 board evidence is pending; no new performance
qualification is claimed. Loader R84 remains 51/51 PASS, final original Flash
restoration remains pending, and GizClaw remains excluded.

CI for `f5e8047a` reports 912/913 tests passing with `pal-core:browser_test`
failing after 90.6 seconds. A fresh local real-Chromium execution passes in
2.4 seconds; this does not erase the CI failure or establish its cause.

### R103/R104 and ACK-driven retransmission restart (2026-10-14)

R103 keeps all board diagnostics off and compares actual Pion media DSCP 46
against data DSCP 0 on the same private AP fixture. Its first boot fails:
unloaded/loaded audio p99 171.746/192.704 ms and 100/100 frames pass, but
fairness **589 < 800** fails. Loaded upload is 57,176 B/s versus download
174,735 B/s. Socket marking/readback and successful write counters are
verified with zero write errors. This does not establish default-fixture
acceptance, nor that the AP honors markings. An earlier controller attempt
referenced the old R101 image: it was cancelled, returned to public Loader,
and is preserved separately as an invalid partial attempt. Only the corrected
source-bound R103 run supplies these metrics.

R104 adds a default-off, bounded 1 Hz SCTP state recorder. It emits only when
the association closes and records no payload. The actual diagnostic boot
fails loaded p99 (260.737 ms; frames 100/99; fairness 931). All 494 samples fit
without overflow. The association reports **274 timeout window reductions**
and 19 fast-recovery entries. With more than 10 KiB queued, median cwnd is
8099 bytes, peer receive window never falls below 1,036,864 bytes, and only
3/328 samples show a pending local emit. These observations motivate examining
timeout behavior rather than claiming that the peer receive window is full.
Two short host test builds/runs overlap its initial data-only phase; the
private overlap receipt explicitly excludes R104 from clean acceptance or
unconfounded throughput comparison. Both runs return to the public R84 Loader.

A deterministic regression reproduces an actual timer defect: after the SACK
for the earliest outstanding TSN, another fragment retransmits at 1500 ms
instead of honoring the restart through 1600 ms. That premature timeout
shrinks cwnd and doubles RTO. The repair retains an ACK-triggered lower bound
on retransmission deadlines using the RTO at restart, without changing any
smoke limits. Duplicate ACKs and later gap ACKs cannot postpone a genuine
loss; cumulative and earliest-gap ACKs work across TSN wrap. A fresh flight
clears the old bound. The existing RTT/Karn test now waits until its correct
restarted deadline (500 ms instead of 410 ms), retaining the same ambiguity
assertions. All 36 H2Peer/SCTP tests pass with diagnostics both enabled and
disabled. This fixes the reproduced premature timeout; it is not a claim of
complete SCTP timer redesign or proven board performance recovery.

R105 default-off two-boot hardware acceptance remains pending. The real peer
will return to default DSCP 0; private AP selection and all original workload,
radio, queue, frame and latency/fairness predicates stay unchanged. Loader R84
remains 51/51 PASS, original full Flash restoration remains pending, and
GizClaw remains outside acceptance. Private receipts:
`rtc103-performance-result.json`, `rtc104-performance-diagnostic-result.json`,
`rtc104-diagnostic-host-overlap.json`, and `rtc105-t3-restart-red.log` plus
`rtc105-full-{enabled,default}-green.log`.

### R105 original smoke acceptance (2026-10-14)

Two independent actual boots pass from committed repair source
`6c0cb80fa267b7a3d60ccb849d0c7f141f132e0f`. All five optional diagnostics are
absent from the native image. The real Pion peer uses its default unwrapped
UDP socket with media/data DSCP 0. The board uses the recorded private channel-6
AP selection in RAM; host network settings remain unchanged and same-AP identity
is not asserted. All original frame, latency, fairness, transfer, priority,
queue and radio-policy requirements remain intact.

| Actual boot | Unloaded audio P99 | Loaded audio P99 | Frames received | Loaded/data throughput | Upload | Download |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | 99.991 ms | 186.422 ms | 100 + 100 | 137.9% | 188,561 B/s | 217,801 B/s |
| 1 | 99.821 ms | 176.737 ms | 100 + 100 | 99.8% | 144,282 B/s | 184,109 B/s |

Both boots have zero missing, duplicate or reordered frames. Native admission,
confirmation and screen completion pass; actual peer witnesses cover both full
workloads. The pre-test R84 P1 is preserved, crash storage remains blank, and
the device returns to its public Loader. The acceptance applies to the recorded
fixture and artifacts, not every AP or acoustic playback. Previous failed and
diagnostic runs remain failures; they are not averaged into this result.

Native image SHA-256:
`e2e9fd6ee2e5b7f66b393d810064acc7b54a11e92aca73b4dd0b1bb8179e8a78`.
Private source/configuration/package/peer binding and complete receipt:
`rtc105-20261014-1791970094521214000-webrtc-performance-qualification.json`.
Original 16 MiB backup size, SHA and mode were reverified before the final
restoration recorded below. The older pre-R84 restoration remains separately
archived.

### Final original Flash restoration after R105 (2026-10-14)

After both R105 boots and the R84 51-case matrix passed, the operator entered
ROM download mode. The tool verified ESP32-S31 and the expected device MAC,
validated the original backup, wrote all 16,777,216 bytes, then independently
read the entire Flash and verified exact byte equality. Restored SHA-256:
`ff0af503fff49d23ce6c9fd016eb7e26975ba4ca8aaf3dd6dab2bf1c1bfed170`.

After reset, the ordinary public Loader reports the expected Mosaico identity,
original P1 version `mosaico-workload-20261012-r42` and matching package/image
checksums, running/next partition 1, empty Stage and last result 0. Crash storage
is blank with zero stored bytes. The final device is back on its original layout;
the temporary R84 Loader and R105 test App are no longer installed. Test evidence
remains bound to the tested artifacts rather than the restored firmware.

Private receipts: `board77-restore-readback.json` and
`board77-original-layout-restored.json`. The earlier pre-R84 restoration is
retained separately under `board77-first-restore-before-loader84/`; it was not
reused as proof of this final restoration. WebRTC performance and Loader recovery
are complete for the recorded scope. GizClaw remains excluded and PR #709 stays
Draft; remote CI and ownership approval are separate from board qualification.
