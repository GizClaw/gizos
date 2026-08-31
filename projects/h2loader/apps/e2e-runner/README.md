# H2Loader E2E Runner

This host-side App runs the same bounded H2Loader acceptance sequence over an
explicit UART endpoint, BLE endpoint, or both. It records every case separately
and never treats a build or unit-test result as physical-device evidence.

The App owns transport-neutral orchestration. Desktop argument parsing,
Bluetooth startup, local firmware loading, console presentation, and JSON
report files belong to the `cc_binary` Target.

Build or run the desktop Target with flags after Bazel's `--` separator:

```sh
bazel run //projects/h2loader/targets/cc_binary/e2e-runner -- \
  --uart /dev/cu.usbmodem11401 \
  --ble-id <endpoint-returned-by-scan> \
  --expected-board devkit \
  --expected-target esp32s3 \
  --app-firmware /absolute/path/devkit-app-esp32s3.update.tar.zlib \
  --loader-firmware /absolute/path/devkit-loader-esp32s3.update.tar.zlib \
  --crash-firmware /absolute/path/devkit-crash-before-confirm-esp32s3.update.tar.zlib \
  --firmware-url http://192.168.1.2:8766/update.tar.zlib \
  --url-bytes 938442 \
  --url-sha256 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef \
  --wifi-ssid TEST-NETWORK \
  --wifi-password-env H2LOADER_E2E_WIFI_PASSWORD \
  --coredump-bytes 16384 \
  --report /tmp/h2loader-e2e.json
```

The runner always executes `help`, `status`, and `stats`; it executes `memory`
only when the authoritative `status.command_availability` advertises that
conditional command. Supplying Wi-Fi credentials enables
`scan`, `connect`, and idempotent `disconnect`; `--app-firmware` enables direct
Stage plus abort, while adding `--loader-firmware` enables the complete APP and
Loader dual-partition lifecycle. When lifecycle and URL inputs are both present,
the runner repeats the command surface, Wi-Fi cases, payload Stage, URL Stage,
and abort after entering APP, and requires every resulting status to prove the
expected active role. The URL
triplet enables device-side download plus abort. `--crash-firmware` stages the
crash-before-confirm APP once, requires
the platform rollback to return to Loader with the failed APP candidate and
Stage retained, `boot_intent=LOADER`, and a non-empty coredump, then verifies
that coredump over every selected transport before erasing it.
`--coredump-bytes` retains the lower-level mode for an already
preloaded coredump. Both coredump modes consume the dump and are limited to one
iteration. Wi-Fi passwords are read only from the
named environment variable and never written to console output or the JSON
report.

The report contract is UTF-8 JSON with schema string
`h2loader-e2e-report/v1`. Top-level fields are emitted in fixed order:
`schema`, `result`, `rc`, UART endpoint/baud, BLE endpoint, expected identity,
repeat/monitor settings, APP/Loader/URL/coredump identity objects, `summary`,
then execution-ordered `cases`. Each case records transport, iteration, name,
PASS/FAIL plus numeric PAL result, terminal enum, elapsed/acknowledged/total/
output/log byte counts, reconnect attempts, and either `null` or the complete
authoritative status with `device_uid` and Stage/P1/P2 metadata. The first BLE
status locks the UID; every reconnect must return the same UID before role,
partition, Stage, or checksum acceptance continues. Raw logs and Wi-Fi credential
values are never serialized. Missing hardware is a handoff gate, never a fake
case or PASS in this report.

Every transport also executes `legacy-commands-absent`: it requires the exact
new help surface and verifies that the removed restart, rollback, and hold
availability bits are clear. Device command-parser unit tests separately send
the removed spellings and require them to be unroutable; the Host public API
does not regain an arbitrary-string escape hatch for this check.

`--monitor-ms` adds the UART-only `monitor`, `reboot loader --monitor`,
`reboot app --monitor`, and, when lifecycle testing is enabled,
`reboot upgrade --monitor` cases. Monitor output contains only bytes that the
Host transport has classified as non-iKCP serial logs. BLE deliberately has no
monitor case because it does not carry the device's UART log stream.
