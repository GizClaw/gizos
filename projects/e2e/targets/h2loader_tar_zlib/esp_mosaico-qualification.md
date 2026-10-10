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

| Suite | Reference launcher | Mosaico entry | Final-source device status |
| --- | --- | --- | --- |
| pal-core | `pal-core/devkit` | [`pal-core/esp_mosaico`](pal-core/esp_mosaico/BUILD.bazel) | 41/41 on two boots before dual CDC; current artifact pending |
| pal-storage | `pal-storage/devkit` | [`pal-storage/esp_mosaico`](pal-storage/esp_mosaico/BUILD.bazel) | NOT_RUN |
| pal-crypto | `pal-crypto/devkit` | [`pal-crypto/esp_mosaico`](pal-crypto/esp_mosaico/BUILD.bazel) | NOT_RUN |
| pal-json | `pal-json/devkit` | [`pal-json/esp_mosaico`](pal-json/esp_mosaico/BUILD.bazel) | NOT_RUN |
| pal-http | `pal-http/devkit` | [`pal-http/esp_mosaico`](pal-http/esp_mosaico/BUILD.bazel) | NOT_RUN |
| pal-mqtt | `pal-mqtt/devkit` | [`pal-mqtt/esp_mosaico`](pal-mqtt/esp_mosaico/BUILD.bazel) | NOT_RUN |
| pal-net-tls | `pal-net-tls/devkit` | [`pal-net-tls/esp_mosaico`](pal-net-tls/esp_mosaico/BUILD.bazel) | NOT_RUN |
| pal-wifi | `pal-wifi/devkit` | [`pal-wifi/esp_mosaico`](pal-wifi/esp_mosaico/BUILD.bazel) | NOT_RUN |
| pal-webrtc | `pal-webrtc/devkit` | [`pal-webrtc/esp_mosaico`](pal-webrtc/esp_mosaico/BUILD.bazel) | NOT_RUN |
| pal-audio | `pal-audio/amoled` | [`pal-audio/esp_mosaico`](pal-audio/esp_mosaico/BUILD.bazel) | NOT_RUN |
| pal-audio-decoder | `pal-audio-decoder/devkit` | [`pal-audio-decoder/esp_mosaico`](pal-audio-decoder/esp_mosaico/BUILD.bazel) | NOT_RUN |
| pal-display | `pal-display/amoled` | [`pal-display/esp_mosaico`](pal-display/esp_mosaico/BUILD.bazel) | NOT_RUN |
| atomic | `atomic/devkit` | [`atomic/esp_mosaico`](atomic/esp_mosaico/BUILD.bazel) | NOT_RUN |
| libco-smoke | `libco-smoke/devkit` | [`libco-smoke/esp_mosaico`](libco-smoke/esp_mosaico/BUILD.bazel) | NOT_RUN |
| lua-link | `lua-link/devkit` | [`lua-link/esp_mosaico`](lua-link/esp_mosaico/BUILD.bazel) | NOT_RUN |
| pal-pref | `pal-pref/devkit` | [`pal-pref/esp_mosaico`](pal-pref/esp_mosaico/BUILD.bazel) | NOT_RUN |

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
  reset reason plus installation/launch stack headroom. Its device verification
  remains pending; stack pressure is a hypothesis until measured.
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
