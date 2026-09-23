# Atomic E2E

The portable app runs the same two-worker workload through `h2_atomic` and
an explicit C11 comparison backend. Each worker performs 100,000 `fetch_add`
operations and 100,000 compare-exchange increments. Each run checks that both
final counters equal 200,000 and prints elapsed microseconds. The C11 backend
exists only in this test app. On ESP, its state is static internal DRAM; the
`h2_atomic` provider also allocates its storage in internal RAM.

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
transport and run `reboot upgrade --monitor`. Require six `H2_ATOMIC_E2E`
lines with `rc=0` and `H2_ATOMIC_E2E_READY rc=0`; then verify `status` reports
the same device UID, `active_role=app`, partition 2, the package version, and
`stage_valid=0`. Timing compares the platform's two implementations under the
same workload; it is not a general atomic performance benchmark.
