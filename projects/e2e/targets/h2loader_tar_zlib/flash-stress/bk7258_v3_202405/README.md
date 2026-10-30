# BK7258 Flash stress diagnostic

This native entry assembles the portable [Flash stress App](../../../../apps/flash-stress/README.md) with BK7258 providers. The App owns the sequential control, concurrent workload, deadlines and verdict. The target owns SDK initialization, the scratch-only [Disk adapter](ap/disk.c), core affinity, UART output and H2Loader management.

The adapter calls the current SDK's real Flash APIs without adding a lock around them. The [task policy](BUILD.bazel) assigns the App's named workers to separate AP cores; the App verifies their actual core IDs before concurrency. Progress uses the platform Atomic provider, whose backing is in SRAM, and no Atomic critical section spans a Disk call. The launcher allocates App state in SRAM and selects SDK default worker stacks. A read mismatch, SDK error, deadline or missing final record remains a failure/incomplete run. A reboot does not count as a completed iteration. SDK watchdog and cross-core heartbeat behavior remain enabled by the selected board configuration.

Only the named `flash_stress` partition is exposed. Its board layout reserves space from the end of the diagnostic App's AP code allocation; Loader, preference and coredump addresses remain unchanged. The adapter checks native partition boundaries and the App checks portable geometry before writing. The reader's seeded sector is immutable during concurrency, so normal writer updates cannot produce false mismatch results. Successful completion joins both workers, erases and verifies the used scratch sectors. A blocked worker retains its owned buffers and the controller never erases beneath it.

The management entry mounts only the board's existing storage and composes a minimal Runtime for UART H2Loader. It does not invoke the board's product constructor or start display/audio/input/modem workers, associate Wi-Fi, or advertise BLE. SDK boot may still initialize its own system/radio services. Installation confirmation is emitted before the test and must not be treated as a test PASS. A successful result requires zero test, cleanup and output errors, joined workers, verified core placement and the matching `run_returned` receipt. The launcher retries destruction after BUSY while preserving the failed run's verdict.

Build the public-board artifact:

```sh
bazel build --config=bk7258 \
  //projects/e2e/targets/h2loader_tar_zlib/flash-stress/bk7258_v3_202405:package
```

Private products reuse this entry's `ap_launcher_component` and the portable App, supplying their own board, task policy and scratch layout in their E2E matrix. No private board is added to the GizOS registry. Install through the current H2Loader; capture both UARTs from before startup. Save original package/image and any dump, and verify restoration independently of the test result. This target is part of the existing BK native build graph; building it never operates a device.
