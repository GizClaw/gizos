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
  --ble-id 4:001122334455 \
  --expected-board devkit \
  --expected-target esp32s3 \
  --firmware /absolute/path/devkit-loader-esp32s3.update.tar.zlib \
  --firmware-url http://192.168.1.2:8766/update.tar.zlib \
  --url-bytes 938442 \
  --url-sha256 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef \
  --wifi-ssid TEST-NETWORK \
  --wifi-password-env H2LOADER_E2E_WIFI_PASSWORD \
  --report /tmp/h2loader-e2e.json
```

The runner always executes `status`. Supplying Wi-Fi credentials enables
`scan`, `connect`, and idempotent `disconnect`; `--firmware` enables direct
stage plus cleanup; the URL triplet enables device-side download plus cleanup.
`--reboot-loader` is deliberately opt-in because it disrupts the active App. Wi-Fi
passwords are read only from the named environment variable and never written
to console output or the JSON report.
