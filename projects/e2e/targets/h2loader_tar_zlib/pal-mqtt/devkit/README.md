# DevKit PAL MQTT E2E

This independent ESP32-S3 App runs the same mandatory 36-case portable registry against the real controlled LAN MQTT broker. It directly owns `h2_coremqtt` using the existing ESP Net/MbedTLS, Time, Log and PSRAM allocator, with 8 incoming and 8 outgoing QoS1 records. The DevKit board's default unsupported MQTT assembly is unchanged. The launcher closes clients, checks actual native resource counters, destroys its provider and checks the provider allocation was released before admitting the App. The SHA256 callback is backed by the existing ESP-IDF MbedTLS component.

Start a fresh LAN fixture with explicit `--bind` and `--advertised` addresses. Preserve its original CA/config files, source, epoch, ports and session prefix. The generated bazelrc embeds those exact public inputs into the managed App; it contains no WiFi credentials. ESP uses the fixture's default bounded 2-second TLS handshake budget. The launcher connects using the device's saved WiFi settings and reports no started cases until it obtains an IPv4 address.

```sh
bazel --bazelrc=/absolute/path/fixture.bazelrc build --config=esp32s3 \
  --//tools/bazel:firmware_version=mqtt-esp-<unique-version> \
  //projects/e2e/targets/h2loader_tar_zlib/pal-mqtt/devkit:package
```

Build evidence alone does not qualify a device. A directed fixture owner must first capture fresh UID/status, the valid original P1 Loader and its hashes, actual coredump bytes, original App and Stage. After managed installation, capture two fresh boots (upgrade and app), each with all 36 cases in order, exact broker CONNECT/TLS/ACK/message witnesses, zero remaining broker clients/retained messages, balanced native resources, successful provider cleanup and confirmation. Then verify P2 validity/version/package/image hashes, empty Stage and unchanged P1/coredump.

The direct `:device_test` only reads those observations. Set `H2_MQTT_DEVICE_EVIDENCE_DIR`, `H2_MQTT_DEVICE_PORT` and `H2_MQTT_DEVICE_UID` explicitly. It is tagged `manual,external`, so an external run cannot reuse a previous test result. It never accesses UART. Restore the original App and Stage after qualification and verify settings and WiFi separately. Browser raw TCP and IPv6 are outside this entry's scope.

Run `:device_test` with the host configuration, for example `--config=macos_arm64 --define=h2_firmware_target=esp32s3`, and the same fixture bazelrc/version. Its package dependency transitions to ESP32-S3 while Python executes on the host. Export the configured IDF toolchain and pass `--repo_env=IDF_PATH --repo_env=IDF_TOOLS_PATH --repo_env=H2_NATIVE_CCACHE_RUNTIME_ROOT`; keep the native and Bazel caches enabled. Qualification must bind the package actually used by the directed installation, including its unchanged SHA256.
