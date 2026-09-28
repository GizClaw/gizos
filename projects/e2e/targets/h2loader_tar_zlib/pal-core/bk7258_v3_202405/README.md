# BK7258 PAL Core E2E

This independent App runs the same complete Core v2 contract used on DevKit: 41 mandatory cases covering the 46 operations in nine Core vtables. Atomic is a Runtime prerequisite, and remains a separate package from PAL.

## Hardware and build

The current fixture is board `bk7258_v3_202405`, UID `c8478ca2a87c`:

- UART1/AP/H2Loader: `/dev/cu.usbserial-20131240`, 460800 8N1.
- UART0/CP/standard port: `/dev/cu.usbserial-20131230`, 460800 8N1.

Recheck ports and device UID before any later run; these paths describe this session's enumeration. UART0 RTS is connected to CEN, confirmed by the user; an asserted/deasserted pulse recovered the unconfirmed App to Loader in this session.

```sh
BK7258_PATH=/path/to/pinned/bk-avdk-smp \
  bazel build --config=bk7258 --lockfile_mode=off \
  --//tools/bazel:firmware_version=<unique-version> \
  //projects/e2e/targets/h2loader_tar_zlib/pal-core/bk7258_v3_202405:package
```

Keep Bazel and native compiler caches enabled. SDK revision is pinned by `tools/bazel/native_versions/bk7258_sdk_commit.txt`. Install the managed App package through UART1 using H2Loader `send`, verify the staged package/image checksums, then use `reboot upgrade --monitor`. The 5,222,400-byte image is linked for the independent App window; normal installation preserves Loader P1. Do not raw flash the App's combined image.

## Observations and acceptance

The launcher uses actual BK providers and native resource counters. Task stack failure injection changes the allocator passed to FreeRTOS's real static-task constructor; it does not replace start/join. Each test worker queries the native stack bounds and checks a stack-local address. Native Task join waits until the worker is off both AP CPUs, deletes its kernel state and completion semaphore, and then releases its owned stack/handle.

Core Memory observations account SDK-reported live allocation sizes. UART transport packet buffers and management-client metadata reads use a separate SDK allocator so asynchronous protocol traffic cannot perturb Core's baseline. Timer callbacks use native workers and a reaper for self-destruction. The event cases exclusively own H2 subscriptions; Runtime's unrelated event capability is canonical unsupported. Logs are observed at the actual SDK logging sink, and expected firmware version is compiled independently into the launcher from build metadata.

BK milliseconds and microseconds use the same AON counter. If the RTC is not calibrated, the launcher first checks UNCALIBRATED behavior and seeds the real RTC from image build UTC. The Core wall-clock case then exercises set/read and restores that baseline. Build UTC is a test fixture, not a time-synchronization accuracy claim.

Before Runtime initialization, two native workers pinned to AP logical CPU0 and CPU1 (SDK-reported chip IDs 1 and 2) must cross an Atomic barrier without yielding and collectively increment to 100,000. This verifies the newly enabled BK Atomic prerequisite separately from the unchanged 41-case PAL contract.

Qualification requires all 41 unique `H2_BK_CORE_CASE` IDs from the current `H2_BK_CORE_BOOT version=...`, plus:

```text
H2_BK_CORE_REPORT contract=2 passed=41 failed=0 blocked=0 not_run=0 complete=1 qualified=1 cleanup=0 rc=0 confirm=0
```

The test runs once per boot, then replays immutable records slowly. Replays are not additional runs. Read back UID, active image checksum/version, P2 validity, Stage cleanup and Loader P1 identity afterward. Compare coredump evidence with the pre-run baseline; this fixture already contained a 32-byte record.

## Verified result (2026-09-28)

`pal-core-bk-20260928-r7` passed **41/41 twice**, on managed installation and again after a normal App reboot: **0 FAIL, 0 BLOCKED, 0 NOT_RUN, cleanup=0, qualified=1, confirm=0**. The native 4/16/64 KiB task stacks and allocation-failure recovery passed. The separate Atomic prerequisite reported physical cores 1/2, 100,000/100,000 increments and no-yield barrier success.

Resource snapshots match exactly before/after Core: 2 persistent PAL Tasks, 98,304 stack bytes, 4 queues, 2 mutexes, 0 semaphores/conditions/timers, 1 static firmware-info provider, and 20 PAL allocations totaling 139,326 bytes. These persistent Runtime/management resources are the baseline, not leaks. The Timer churn fixture held one additional queue while its 100 cycles ran; its resource snapshot remained unchanged after every destroy.

[The qualification ledger](evidence/2026-09-28/qualified.json) binds both runs to the registry, image manifest and device UID. The normal-reboot CLI monitor exited early with a host timeout after recording CP/App startup and the Atomic prerequisite. The device continued running; live status and the immutable post-reboot replay supplied all 41 case records and the successful summary. This is recorded separately from the uninterrupted install-run capture.

The active App image SHA is `374b046ddaaa3aa1e508c08d24d98407b0daa2e04cb1e79995dc7d25f32727bc`. Loader P1 remains `0.1.65-lu-c`; final P2 is valid, next boot is P2, Stage is empty and Loader last_result is zero. The pre-existing 32-byte coredump was preserved and compared byte-for-byte; this is not a claim that the partition was blank. The structured ledger records installation, status, coredump comparison and the two qualified runs; raw transport captures remain local diagnostic artifacts.

## Changes and earlier evidence

- Added native Timer and real resource observations, fixed Task completion semaphore reclamation and joined native-worker lifetime, and added the actual PSRAM-stack allocator hook. Stack sizes round up to a word boundary and SDK depth overflow is rejected.
- Queue close now retains buffered data and rejects pending senders; reset and latest-send coordinate with concurrent queue operations.
- Millisecond and microsecond Time operations share the AON clock domain.
- Implemented the missing BK Atomic provider required by Runtime, using SRAM storage and the SDK's cross-core critical section. Host API regressions and the independent real dual-core prerequisite check both passed.
- Fixed launcher CPU-ID interpretation, preserved unconfirmed rollback evidence on early failure, and used the same Runtime instance for command management and App confirmation.
- R5/R6 exposed a 456-byte temporary Loader metadata allocation in Core snapshots during host status polling. Client and transport allocations are now both isolated, preserving all strict Core comparisons.

Earlier failed attempts are summarized here for diagnostic context. R1 never started Core because Runtime's Atomic backend returned UNSUPPORTED. R3's Atomic count was correct but the launcher compared physical IDs to logical IDs. R5/R6 had 39/41 Core passes while the resource observer included management allocations.
