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
  --app-firmware /absolute/path/devkit-app-esp32s3.update.tar \
  --loader-firmware /absolute/path/devkit-loader-esp32s3.update.tar \
  --crash-firmware /absolute/path/devkit-crash-before-confirm-esp32s3.update.tar \
  --firmware-url http://192.168.1.2:8766/update.tar \
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
the platform rollback to return to Loader without changing the Stage,
Partition 2 metadata, package, boot intent, or last result, and with a non-empty
coredump, then verifies that coredump over every selected transport before
erasing it.
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

The optional `on_log` / `log_user` sink is borrowed for the run and called
synchronously on the runner thread. It receives non-frame UART bytes from every
connection and BLE connect-failure diagnostics. UART sink errors abort the current
serial operation; BLE sink errors never replace the connect error. Serial log
bytes contribute to output/log byte counters only during monitor cases.

`reboot ... --monitor` allows one CLOSED/TIMEOUT reset transition after the ACK.
Each attempt reconnects to the expected partition, observes a full monitor window
with output, and reads live status to confirm the partition. A second transition,
any other error, a wrong partition, or cancellation ends the case.


## Old/new checksum matrix

Build two distinct command-responsive, confirming Apps for the same board. The
DevKit fixture target shares the original launcher and uses a separate linked
version for its alternate App:

```sh
bazel build --config=esp32s3 //projects/h2loader/targets/h2loader_tar_zlib/e2e-app/devkit:checksum-matrix
bazel run --config=macos_arm64 //projects/h2loader/targets/cc_binary/e2e-runner -- \
  --uart <directed-port> --expected-board devkit --expected-target esp32s3 \
  --checksum-tar-zlib <absolute-checksum-matrix-dir>/tar_zlib \
  --checksum-zlib-tar <absolute-checksum-matrix-dir>/zlib_tar \
  --report /tmp/h2loader-checksum-e2e.json
```

Either format directory may be selected independently; both compare identical
App/data identities through old/new wire formats. Each directory must contain
`baseline`, `unchanged`, `app-only`, `data-only`, `both-changed`, with suffix
`.update.tar.zlib` for format 1 or `.update.tar` for format 2. The sequence is
A/A baseline → A/A unchanged → B/A App-only → B/B data-only → A/A both-changed.
Every repeat and selected UART/BLE transport establishes its own baseline.
These controlled fixtures replace the App and data tree with the E2E App and
`data/h2loader-e2e/probe.txt`; use them on the explicitly selected test device.
The runner's endpoint flags own device selection and do not authorize a global scan.

Before any connection, inspect every packet, verify its format/role/board/target,
canonical data identity, four change relations, and old/new pair agreement.
Format-2 unchanged has two invalid zlib streams, App-only invalidates data,
and data-only invalidates App. Their compressed SHA-256 fields and complete
package identities remain correct. The runner checks the guards too: valid
unguarded packets cannot report skip proof. A successful device install proves
those skipped streams were not inflated. All changed streams are valid.

A current Loader/App command implementation must return the separate
`H2_LOADER_DATA_CHECKSUM` fact from `stats`; older or unsupported implementations
fail before the first baseline mutation. Before each transition compare the
observed App/data checksums with the previous successful fixture, then use the
normal managed Stage/activate/reconnect flow. Afterward require the expected
active App/package metadata, Partition 2, no Stage, and the expected installed
data checksum. Data-only success with the old data checksum fails. A failed
matrix case blocks its dependent cases; the other format starts its own baseline.

Checksum runs use `h2loader-e2e-report/v2`; normal runs retain v1. Each checksum
case records its immutable package SHA, format, observed before/after checksums,
expected update flags and verified status. Baseline expectations are null because
it establishes the initial state. The elapsed time covers the full managed
operation and observation, not an isolated decompression benchmark. Host tests
exercise recipes, orchestration and negative acceptance; hardware PASS requires
running these real cases on the directed endpoints and retaining their report.
