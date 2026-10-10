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
| pal-core | `pal-core/devkit` | [`pal-core/esp_mosaico`](pal-core/esp_mosaico/BUILD.bazel) | NOT_RUN |
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

## Remaining hardware and integration gates

- Board diagnostics: rerun display/touch/buttons, both magnetometers, BMI270,
  read-only battery, audio and camera on the final artifact. Observe actual sound
  and pixels separately from successful API calls. Camera insertion needs its real
  empty-slot-to-insert-to-capture sequence; live removal remains unsupported.
- Managed H2Loader transport: these entries use the existing board UART console
  profile. Mosaico's main Type-C TinyUSB diagnostic console is not proof of a
  working managed H2Loader serial channel. The connected test board currently has
  Type-C only; its unrelated USB-UART port must not be used. Core additionally
  routes its diagnostic ledger through TinyUSB CDC for direct-flash investigation;
  that does not qualify managed installation or recovery. Validate the actual physical transport,
  fresh UID and protocol before installation; do not silently select a ROM port.
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

Change the suite segment for the entries above. Build success is a separate field
from execution. After device/transport/fixture validation, follow the corresponding
shared suite's device procedure and collect the actual first execution, immutable
package binding, independent restart, resource cleanup and peer witness receipts.
For MQTT, the host verifier accepts only the exact `esp_mosaico` / `esp32s31` pair
and enforces the same ESP provider cleanup and confirmation contract as DevKit.

Do not mark PR #709 ready or close Issue #708 based on this file alone. Record real
results and exact artifacts after execution; keep unavailable hardware/fixtures
visible as blockers. Never populate evidence with the reference board's receipts.
