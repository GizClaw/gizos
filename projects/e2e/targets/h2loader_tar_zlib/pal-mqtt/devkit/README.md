# DevKit PAL MQTT E2E

This independent ESP32-S3 App runs the same mandatory 36-case portable registry against the real controlled LAN MQTT broker. It directly owns `h2_coremqtt` using the existing ESP Net/MbedTLS, Time, Log and PSRAM allocator, with 8 incoming and 8 outgoing QoS1 records. The DevKit board's default unsupported MQTT assembly is unchanged. The launcher closes clients, checks actual native resource counters, destroys its provider and checks the provider allocation was released before admitting the App. The SHA256 callback is backed by the existing ESP-IDF MbedTLS component.

Start a fresh LAN fixture with explicit `--bind` and `--advertised` addresses. Preserve its original CA/config files, source, epoch, ports and session prefix. The generated bazelrc embeds those exact public inputs into the managed App; it contains no WiFi credentials. ESP uses the fixture's default bounded 2-second TLS handshake budget. The launcher connects using the device's saved WiFi settings and reports no started cases until it obtains an IPv4 address.

```sh
bazel --bazelrc=/absolute/path/fixture.bazelrc build --config=esp32s3 \
  --//tools/bazel:firmware_version=mqtt-esp-<unique-version> \
  //projects/e2e/targets/h2loader_tar_zlib/pal-mqtt/devkit:package
```

Build evidence alone does not qualify a device. A directed fixture owner must first capture fresh UID/status, the valid original P1 Loader and its hashes, actual coredump bytes, original App and Stage. After managed installation, capture two fresh boots (upgrade and app), each with all 36 cases in order, exact broker CONNECT/TLS/ACK/message witnesses, zero remaining broker clients/retained messages, balanced native resources, successful provider cleanup and confirmation. Then verify P2 validity/version/package/image hashes, empty Stage and unchanged P1/coredump.

The direct `:device_test` only reads those observations. Set `H2_MQTT_DEVICE_EVIDENCE_DIR`, `H2_MQTT_DEVICE_PORT`, `H2_MQTT_DEVICE_UID` and `H2_MQTT_DEVICE_PACKAGE` explicitly; the last path must be the immutable managed package actually installed. Preserve `package-binding.json` with its source commit, package/manifest and exact fixture inputs. It is tagged `manual,external`, so an external run cannot reuse a previous test result. It never accesses UART. Restore the original App and Stage after qualification and verify settings and WiFi separately. Browser raw TCP and IPv6 are outside this entry's scope.

Run `:device_test` with the host configuration, for example `--config=macos_arm64`. It consumes the observed package without rebuilding firmware. A later build with a different image hash cannot inherit the old board qualification. Build `:package` separately using the configured IDF toolchain and enabled native/Bazel caches.

The latest qualification is recorded in `evidence/runs/cd3dda1d`: fixed App/fixture source `cd3dda1d`, two fresh 36/36 ledgers, retained received=2 and exact wire publish=2/disconnect=1, native resource balance, provider cleanup and a newly executed host verifier. Original R35 App, nonempty Stage, WiFi, blank coredump and empty Data were restored while preserving the upgraded dual-format P1. Failed restore host observations and the subsequent directed reset/reboot/final-success receipts remain distinct. Earlier `955dcfce` and `da7704d9` executions retain their actual source and artifact identities.

Current launcher borrows the existing USB JTAG PAL installed by H2Loader to emit one RUN/36 CASE/SUMMARY/READY sequence per boot. Each complete record uses one bounded write with its actual byte count and TX flush checked. Only after the whole first ledger and READY(confirm=pending) have been delivered does it persistently confirm the App and emit CONFIRMED with the actual result, then hold with management available. Partial/write/flush failure prevents confirmation; replay or a later startup cannot supply a missing first ledger. New fixture receipts bind the distinct CA/hostname rejection alerts and actual observed server names, rejecting absent/null SNI. Older cd3dda1d receipts retain their original source/verifier schema and are not rewritten as current-head runs.
