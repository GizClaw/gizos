# Atomic E2E

The portable app runs the same two-worker workload through `h2_atomic` and
an explicit C11 comparison backend. Each worker performs `fetch_add` and
compare-exchange increments. The test checks both final counters and prints
elapsed microseconds. The C11 backend exists only in this test app.

Run on Desktop:

```sh
bazel test --config=macos //projects/e2e/targets/cc_test/atomic:atomic_e2e_test
```

Run in WebAssembly:

```sh
bazel test --config=macos //projects/e2e/targets/pkg_tar/atomic:atomic_wasm_test
```

The current Web PAL schedules tasks cooperatively. Its output marks
`concurrent=SKIP`; it verifies both implementations' sequential behavior but
does not claim a browser concurrency result.

Build the managed ESP32-S3 DevKit App package:

```sh
bazel build --config=esp32s3 \
  //projects/e2e/targets/h2loader_tar_zlib/atomic/devkit:package
```

Install the `.update.tar.zlib` package through the DevKit's H2Loader command
transport and run `reboot upgrade --monitor`. The ESP app pins its workers to
CPU0 and CPU1, synchronizes their start, and runs each backend three times in
internal RAM (100,000 operations of each kind per worker) and PSRAM (20,000
operations of each kind per worker). It prints the observed cores, counters,
CAS retry failures, wrapper and storage addresses, memory placement checks,
elapsed time, and verdict for every run. The C11 atomic values are in PSRAM
for PSRAM runs; the `h2_atomic` wrappers are in PSRAM while their provider
storage is in internal RAM. A failed comparison is logged and included in
`H2_ATOMIC_E2E_READY aggregate_failures=...`; the app still confirms its
H2Loader installation. Verify `status` reports the same device UID,
`active_role=app`, partition 2, the package version, and `stage_valid=0`.
Timing compares the platform's two implementations under this workload; it is
not a general atomic performance benchmark.

On DevKit UID `9888e0115c52` with ESP-IDF 6.0.3, the internal RAM runs passed
for both backends. All three PSRAM `h2_atomic` runs reached the expected
40,000 increments and 40,000 compare-exchange increments. All three PSRAM
C11 runs failed with lost counts. For example, one run ended with 28,533 and
24,679 respectively against the expected 40,000; both workers were observed
on separate cores and the C11 state address was verified as PSRAM.
