# BK7258 Flash stress diagnostic

This native E2E entry repeatedly erases, writes and verifies internal Flash on one AP core while the other AP core continuously reads a separately seeded sector. It calls the current SDK's real Flash APIs. It is a backend diagnostic, not the portable PAL Storage qualification and not H106 Main.

The executable and its thresholds live in [flash_stress.c](ap/flash_stress.c). It first performs a sequential control, verifies actual worker affinity, then runs the concurrent phase. A read mismatch, SDK error, deadline or missing final record remains a failure/incomplete run. A reboot does not count as a completed iteration. SDK watchdog and cross-core heartbeat behavior remain enabled by the selected board configuration.

Only the named `flash_stress` partition is used. Its board layout reserves space from the end of the diagnostic App's AP code allocation; Loader, preference and coredump addresses remain unchanged. Runtime checks verify the named region and adjacency before writing. The reader's seeded sector is immutable during concurrency, so normal writer updates cannot produce false mismatch results. Successful completion erases and verifies the used scratch sectors. A blocked worker retains its static buffers and the controller never erases beneath it.

The management entry mounts only the board's existing storage and composes a minimal Runtime for UART H2Loader. It does not invoke the board's product constructor or start display/audio/input/modem workers, associate Wi-Fi, or advertise BLE. SDK boot may still initialize its own system/radio services. Installation confirmation is emitted before the test and must not be treated as a test PASS. The workers remain parked after their terminal result; this diagnostic does not qualify task teardown.

Build the public-board artifact:

```sh
bazel build --config=bk7258 \
  //projects/e2e/targets/h2loader_tar_zlib/flash-stress/bk7258_v3_202405:package
```

Private products consume `bk7258_flash_stress` with their own board and explicit layout files. No private board is added to the GizOS registry. Install through the current H2Loader; capture both UARTs from before startup. Save original package/image and any dump, and verify restoration independently of the test result. This target is part of the existing BK native build graph; building it never operates a device.
