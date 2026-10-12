# ESP-Mosaico E2E qualification

Status: **NOT QUALIFIED**. PR #709 remains draft until all agreed mandatory device
suites pass against the final source and immutable firmware artifacts. Historical
bring-up receipts and green compile/host CI are not device qualification.

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
| pal-audio | `pal-audio/amoled` | [`pal-audio/esp_mosaico`](pal-audio/esp_mosaico/BUILD.bazel) | r32 24/24 on two independent boots; actual capture/output and >=30-second soaks, cleanup/confirmation/screens PASS; current acoustic observation unanswered |
| pal-audio-decoder | `pal-audio-decoder/devkit` | [`pal-audio-decoder/esp_mosaico`](pal-audio-decoder/esp_mosaico/BUILD.bazel) | r32 29/29 on two independent boots; cleanup/confirmation/screens and managed install PASS |
| pal-display | `pal-display/amoled` | [`pal-display/esp_mosaico`](pal-display/esp_mosaico/BUILD.bazel) | r32 24/24 on two independent boots; original DMA/pattern/dimming plus STARTING/CONFIRMING/PASS rendering PASS; operator stage/pixels observations retain their separate r31/r28 bindings |
| atomic | `atomic/devkit` | [`atomic/esp_mosaico`](atomic/esp_mosaico/BUILD.bazel) | r32 56/56 on two independent executions, all 20 workers joined; cleanup/confirmation/screens and managed install PASS |
| libco-smoke | `libco-smoke/devkit` | [`libco-smoke/esp_mosaico`](libco-smoke/esp_mosaico/BUILD.bazel) | r32 six phases/10,000 switches on ten independent boots; confirmation/screens, managed install and actual original dump preservation PASS |
| lua-link | `lua-link/devkit` | [`lua-link/esp_mosaico`](lua-link/esp_mosaico/BUILD.bazel) | r32 host/r6 AMOLED join: five full bilateral rounds, confirmed identities and connected hold; operator PWR cycle gives actual lost:-10 / 4100 ms / 1766 ticks, both exact public Loader returns and dump preservation PASS |
| pal-pref | `pal-pref/devkit` | [`pal-pref/esp_mosaico`](pal-pref/esp_mosaico/BUILD.bazel) | r32 fresh 31 operations over three boots plus independent empty 1 recheck; image reset/persistence/confirmation/screens PASS after missing-ledger interruption; only private fixture prepared using distinct archived r28 image |

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
