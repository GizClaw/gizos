# AMOLED GizClaw OTA hardware acceptance

The explicit `H2_GIZCLAW_E2E_OTA_ONLY` lane installs a real AMOLED App package. It is separate from the ordinary device API suite, whose Stage sink deliberately rejects installation. Use only on an operator-authorized AMOLED test device.

Build the same launcher twice with distinct versions containing `source` for the starting image and `target` for the destination. Select the isolated public test fixture explicitly. CMake accepts only the literal `amoled-ota-e2e-20260906`; missing values, `deploy-default`, and arbitrary private tokens fail the OTA-only build. This is intentionally public, disposable E2E registration, following the existing launcher public-fixture convention; it must bind only the isolated test Firmware and must never grant production access. Private registration credentials are not supported build inputs. The launcher derives the device serial from the Bluetooth MAC using the same identity rule as H2Loader and reuses the existing E2E endpoint configuration:

```sh
bazel build --config=esp32s3 \
  --define=H2_GIZCLAW_E2E_OTA_ONLY=1 \
  --define=H2_GIZCLAW_E2E_OTA_TOKEN=amoled-ota-e2e-20260906 \
  --//tools/bazel:firmware_version=0.1.1-ota-target \
  //projects/e2e/targets/h2loader_tar_zlib/gizclaw-e2e/amoled:package
# Preserve the target package before building the source version.
bazel build --config=esp32s3 \
  --define=H2_GIZCLAW_E2E_OTA_ONLY=1 \
  --define=H2_GIZCLAW_E2E_OTA_TOKEN=amoled-ota-e2e-20260906 \
  --//tools/bazel:firmware_version=0.1.1-ota-source \
  //projects/e2e/targets/h2loader_tar_zlib/gizclaw-e2e/amoled:package
```

Upload the target package to the E2E distribution store, verify an anonymous HTTPS download through `tos-accelerate.volces.com` or the configured CDN against its SHA-256 and size, and bind it to the `develop` slot of the isolated Firmware `amoled-ota-e2e-20260906`. The equally named RegistrationToken binds that Firmware and RuntimeProfile `default`. Do not replace the shared `deploy-default` fixture or product channels.

Use H2Loader managed `send` and `reboot upgrade` to install the **source** package. The launcher publishes the SNTP-calibrated SDK wall clock through Runtime Time PAL before connecting, so signed requests have valid UTC timestamps. It uses the device's saved Wi-Fi configuration, persists a generated Peer identity in its own NVS namespace, registers through the deployed Beijing Edge, creates a device-scoped API key, and sends authenticated HTTP `POST /gizclaw/v1/device/actions/firmware-update`. This traverses the deployed control plane and delivers `client.firmware.update` back to the physical device. The HTTP response is only command acceptance. The hardware lane allows 180 seconds for downloads, including flash write time.

The real Stage adapter streams bytes into `/dl/update.tar.zlib`, verifies size, SHA-256, manifest, board, target and App role, saves the attempt ID and target image identity, then commits valid Stage metadata. The runner joins GizClaw before asking H2Loader to reboot and install. The new image reconnects with the same Peer identity and reports `succeeded` only after matching the saved target version/SHA-256 and verifying Stage is cleared. This adapter belongs to the test launcher; this acceptance does not certify an unrelated product's OTA adapter.

Collect serial `H2_AMOLED_OTA` records, authenticated device status snapshots, and H2Loader status. Admin Peer runtime is useful for connectivity, but does not expose the persisted OTA snapshot. Require the same `update_id` across progress and success, a changed active image checksum matching the target manifest, target version in Partition 2, valid Loader recovery partition, cleared Stage, zero last error, and no new coredump. The target reads authenticated `/device/status` and requires the same attempt, version and 100% progress in a persisted `succeeded` snapshot before emitting `FULL_CHAIN_PASS`; it then revokes the test API key. Transport acceptance of telemetry alone is not proof of server persistence.

The lane is intentionally persistent for inspection after the run. Record and clean up only its exact temporary Peer/API key, RegistrationToken, Firmware and uploaded object once evidence collection is complete. Never print private keys, API-key secrets, Wi-Fi credentials or signed URLs.

## Persistent attempt and failure acceptance

The target-local NVS namespace `amoled_ota_e2e` owns the Peer keys, temporary API-key name, update ID, target version/hash and `phase_v1` marker. The marker suffix versions this test-only record. Begin invalidates Stage, commits `downloading`, then records the new update ID. Finish stores target identity, commits valid Stage after package validation, then commits `staged`. Abort records `failed` and invalidates Stage. A power loss before `staged` fails closed: target success is rejected even if an image was installed. Post-boot accepts only `staged` or previously verified `succeeded`, matching running identity, valid recovery/App partitions, clear Stage and zero last result; only a matching persisted server snapshot changes the marker to `succeeded`. Reboot may replay verified success for the same attempt. A new source attempt overwrites the marker before its target identity can be used. Missing/unknown markers require a new source attempt; never erase the shared Wi-Fi preferences to reset this test.

The source waits at most 300 seconds after command acceptance for activation, covering metadata, the 180-second HTTP transfer budget and verification. A new failed attempt observed through device status terminates the source wait early; the pre-action update ID prevents a retained older failure from aborting the new run. Shutdown retries stop at most three times; if joining still fails, terminal evidence explicitly reports retained resources, and borrowed state/NVS stay alive. The outer diagnostic loop does not retry an OTA automatically.

Before accepting changes to failure behavior, use only the isolated fixture to inject a missing develop package, an unreachable HTTPS URL, and a truncated or hash-mismatched package. Require no activation, invalid Stage and terminal failure (within the source deadline), then restore the verified target and rerun. Check partial-stage abort and retry with H2Loader status; never interrupt power while writing the App partition for this acceptance. `FULL_CHAIN_PASS` verifies firmware and server state; the operator separately checks `coredump status` against the pre-run baseline. Tests for simulated join failure require a service fake; retain that limitation explicitly when only hardware acceptance is available.
