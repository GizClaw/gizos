# Flash stress App

This standalone E2E App repeatedly erases, writes and verifies one scratch block while a second task reads an independently seeded, immutable block. The sequential control, data patterns, readiness and deadline handling, progress, verdict and cleanup belong to the [portable App](app/src/h2_flash_stress.c). Its [public API](app/include/h2_flash_stress.h) consumes Disk, Task, Time and Memory PAL providers and the linked Atomic implementation; it does not depend on a board, SDK or H2Loader.

The final target supplies an exclusively reserved scratch partition, providers, task policy and output. An optional core observer lets the App verify actual core placement. Without it, a successful run establishes task concurrency only. No App lock is held across Disk calls. Provider errors, bad data, incomplete counts, output errors or deadline expiry fail the run.

Completed workers are joined before scratch cleanup. A timeout cannot cancel a blocked provider: the instance retains its buffers and dependencies, and destruction reports BUSY until the workers finish. The caller must keep these resources alive and retry destruction. Sequential control and final cleanup are synchronous, so their completion still depends on the Disk provider returning.

Host tests run without SDK or board sources:

```sh
bazel test --config=macos_arm64 \
  //projects/e2e/apps/flash-stress/app:flash_stress_test
```

The [BK7258 target](../../targets/h2loader_tar_zlib/flash-stress/bk7258_v3_202405/README.md) selects the real SDK Flash adapter and cross-core task policy. Native build success and host lifecycle tests do not establish hardware qualification or the cause of historical resets.
