# DevKit carrier device integration

This independent App uses the real ESP Runtime and public Lingxi/OneNET E2E Apps, selecting one or both platforms from runtime configuration. Lingxi performs a device query, one heartbeat and temporary credential cleanup. The heartbeat uses observed Wi-Fi IP, MAC, system and test-firmware versions, with protocol defaults for unmeasured network speed. OneNET connects, subscribes, reports properties, receives the platform response, then closes its client and owned CoreMQTT provider. It binds the acquired Wi-Fi IPv4 address and verifies TLS using Runtime Net's trusted CA. Firmware contains no real credentials or persistent reporting loop. This suite requires real external services and belongs to integration testing.

Prepare a clean ESP-IDF checkout matching the repository's pinned revision, then build the generic artifact without access configuration:

```sh
bazel build --config=esp32s3 \
  --//tools/bazel/firmware:firmware_version=carrier-devkit-<unique-version> \
  //projects/testing/targets/h2_loader_tar_zlib/integration/carrier/espressif/esp32s3_devkitc_1:package
```

Access configuration comes only from the local process environment. Supply a complete configuration for each selected platform; missing fields fail, and platform identities and credentials remain separate. The device constructs its heartbeat from hardware and ignores host heartbeat JSON and source-IP overrides. Read required and optional environment variables from the provisioner's usage:

```sh
bazel run --config=macos_arm64 \
  //projects/testing/targets/h2_loader_tar_zlib/integration/carrier:provision -- --help
```

OneNET accepts a PEM trust certificate obtained from an official operator source through `ONENET_CA_FILE`. Endpoint-name verification is the default. When an older platform certificate uses a service identity, `ONENET_TLS_SERVER_NAME` may specify that expected identity alongside an explicit CA. Verify the official certificate, validity, fingerprint and expected identity before connecting. Verification failure stops execution without changing identity or disabling verification. Real Net PAL still enforces certificate and identity validation; global CA and PAL validation rules remain unchanged. See [OneNET MQTT device connection](https://iot.10086.cn/doc/aiot/fuse/detail/919) for official endpoints and certificates.

Create a private data package in a controlled local macOS/Linux directory. Inputs are the preceding build's `app.bin` and `.firmware.json`, with matching sizes and hashes. The tool only packages; it does not invoke builds, serial access or platform APIs. Environment values do not enter Bazel actions. Output files are owner-readable from creation and cannot overwrite existing files:

```sh
bazel run --config=macos_arm64 \
  //projects/testing/targets/h2_loader_tar_zlib/integration/carrier:provision -- \
  --app-bin /absolute/path/to/app.bin \
  --metadata /absolute/path/to/devkit-carrier-esp32s3.firmware.json \
  --output /absolute/private/directory/carrier.update.tar.zlib
```

The tool uses H2Loader's public artifact writer to place configuration in a one-use Data file. The device deletes the file after reading, clears temporary parsing buffers and clears configuration memory after testing. Tokens are caller-supplied or generated on the board through Crypto PAL from an explicit OneNET Key. Packaging time calibrates TLS and token clocks, so package shortly before testing. Do not commit packages or temporary credentials, upload them to CI or place them in shared build caches. Parsing and budgets are defined by the [runtime input parser](../../../../../../integration/carrier/libs/device/src/h2_carrier_device_inputs.c); environment mapping is in the [provisioner](../../provision.py).

Before installation, verify the selected device's UID, P1/P2, Stage, Data, coredump, and saved Wi-Fi; retain verifiable recovery packages for the original App and Stage. The entry uses existing Wi-Fi configuration without saving changes. Transfer the private package through H2Loader, verify Stage identity and hashes, then perform the managed upgrade and collect fresh BOOT, selection, run ID, case, and terminal results. DNS, TCP, TLS, and MQTT CONNACK observations record only non-sensitive status and do not change provider return values.

Each boot executes the selected platform flows once. Later output replays the same ledger without repeating requests. The App is confirmed only after every selected flow and cleanup succeeds. Failed images remain manageable and unconfirmed. Unselected platforms receive no qualification. Platform business success, network connection, authentication and property-model acceptance are reported separately. Without CONNACK, a general error or timeout cannot establish Key rejection.

Initialization failure after loading configuration clears credentials before reporting the error and remaining manageable. The host `setup_failure_test` executes the same termination path, replacing only the FreeRTOS stay-alive primitive, to verify that active credentials are cleared before entering that state.

After testing, use H2Loader to restore the original App, Stage and Loader boot state, then recheck P1, Data, Wi-Fi and coredump. Final results bind the executed source, image/package hashes and run ID. Builds, loopback tests and other images cannot substitute for this live-service observation. This entry does not issue Jiutian model requests or establish H106 firmware integration or enrollment acceptance.
