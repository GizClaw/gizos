# AMOLED GizClaw OTA hardware acceptance

The explicit `H2_GIZCLAW_E2E_OTA_ONLY` lane installs a real AMOLED App package. It is separate from the ordinary device API suite, whose Stage sink deliberately rejects installation. Use only on an operator-authorized AMOLED test device.

Build the same launcher twice with distinct versions containing `source` for the starting image and `target` for the destination. Supply the isolated registration token explicitly; missing or malformed tokens fail the OTA-only build. The token below is an example fixture identifier, not a product credential. The launcher reads the device serial from H2Loader status and reuses the existing E2E endpoint configuration:

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

Use H2Loader managed `send` and `reboot upgrade` to install the **source** package. It uses the device's saved Wi-Fi configuration, persists a generated Peer identity in its own NVS namespace, registers through the deployed Beijing Edge, creates a device-scoped API key, and sends authenticated HTTP `POST /gizclaw/v1/device/actions/firmware-update`. This traverses the deployed control plane and delivers `client.firmware.update` back to the physical device. The HTTP response is only command acceptance. The hardware lane allows 180 seconds for downloads, including flash write time.

The real Stage adapter streams bytes into `/dl/update.tar.zlib`, verifies size, SHA-256, manifest, board, target and App role, saves the attempt ID and target image identity, then commits valid Stage metadata. The runner joins GizClaw before asking H2Loader to reboot and install. The new image reconnects with the same Peer identity and reports `succeeded` only after matching the saved target version/SHA-256 and verifying Stage is cleared. This adapter belongs to the test launcher; this acceptance does not certify an unrelated product's OTA adapter.

Collect serial `H2_AMOLED_OTA` records, authenticated device status snapshots, and H2Loader status. Admin Peer runtime is useful for connectivity, but does not expose the persisted OTA snapshot. Require the same `update_id` across progress and success, a changed active image checksum matching the target manifest, target version in Partition 2, valid Loader recovery partition, cleared Stage, zero last error, and no new coredump. The target reads authenticated `/device/status` and requires the same attempt, version and 100% progress in a persisted `succeeded` snapshot before emitting `FULL_CHAIN_PASS`; it then revokes the test API key. Transport acceptance of telemetry alone is not proof of server persistence.

The lane is intentionally persistent for inspection after the run. Record and clean up only its exact temporary Peer/API key, RegistrationToken, Firmware and uploaded object once evidence collection is complete. Never print private keys, API-key secrets, Wi-Fi credentials or signed URLs.
