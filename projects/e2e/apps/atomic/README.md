# Atomic E2E

Atomic is an independent library, outside PAL. The portable App uses PAL Mem,
Task and Time only to arrange actual concurrent work and collect results.

The mandatory registry contains 28 cases and exercises all 76 typed functions
in `libs/atomic/include/h2_atomic.h`. `api_coverage.json` maps each function to
its dynamic and file-static cases. Every integer type, bool, pointer and flag
checks initialization, duplicate initialization, destruction and reuse;
file-static destruction preserves storage. Integer cases include load, store,
exchange, successful and failed CAS (including replacement of `expected`),
add, subtract, OR, AND and the C generic conveniences. All valid load/store,
RMW and CAS-failure orders are exercised.

Two real tasks cross a ready/go barrier before contention. They verify dynamic
and static integer counts, CAS counts, flag exclusion of a plain-data critical
section, and release/acquire publication of plain payload through bool and
pointer atomics. Two independent static flags and a dynamically allocated flag
are also exercised. Ten tasks must start and join during each qualification.
A failed bounded join retains heap-owned worker state, reports teardown failure,
and cannot qualify. No failed or unsupported H2Atomic operation counts as PASS.

ESP32-S3 and BK7258 run every case with internal wrappers and with PSRAM wrappers;
backings must remain in internal memory. The board callback observes the actual
wrapper/backing addresses before destruction. CPU0/CPU1 observations and PAL
resource counts are checked. The DevKit additionally checks independently stored
static flags with different-priority CPU0 workers and 20,000 operations each.

Direct C11 is a separate comparison, exercised only in supported storage.
On Xtensa the direct-C11 PSRAM experiment is unsupported and is not executed.
The old 2026-09-25 DevKit run passed six H2Atomic counter samples but lost direct
C11 PSRAM counts (`aggregate_failures=3`); its unconditional confirmation was
not a full qualification. Current board launchers confirm only after every
mandatory case and cleanup succeeds.

Run the desktop and actual Chromium shared-memory Worker tests directly:

```sh
bazel test --config=macos_arm64 \
  //projects/e2e/targets/cc_test/atomic:atomic_e2e_test \
  //projects/e2e/targets/pkg_tar/atomic:atomic_browser_test
```

The existing Node `atomic_wasm_test` remains supplementary. Browser qualification
requires two distinct pthread Worker identities, shared Wasm memory, every
mandatory case and both supported comparison counters.

The iOS and Android consumers use the shared mobile runner. Their BUILD declaration
owns package identities, the exact registry and result schema. The report requires
28/28, 10/10 joined tasks, equal resource snapshots and successful provider shutdown.
These are simulator/emulator qualification, separate from physical phone evidence:

```sh
H2_IOS_SIMULATOR_UDID=<booted-simulator-udid> bazel test --config=ios_sim_arm64 \
  //projects/e2e/targets/ios_application/atomic:ios_atomic_simulator_test
ANDROID_HOME=<sdk> ANDROID_NDK_HOME=<ndk> H2_ANDROID_SERIAL=<booted-emulator-serial> \
  bazel test --config=android_arm64 \
  //projects/e2e/targets/android_binary/atomic:android_atomic_simulator_test
```

Native fixtures require an explicitly handed-off port and UID. Never globally
scan while another suite owns a board. After loading the pinned SDK environment,
run the direct test with host interpreter/test toolchains selected:

```sh
H2_ATOMIC_DEVICE_PORT=<port> H2_ATOMIC_DEVICE_UID=<uid> \
  bazel test --config=esp32s3 \
  --extra_toolchains=//projects/e2e/libs/atomic-device:host_python_toolchain \
  --extra_toolchains=//tools/bazel/platforms:macos_arm64_test_toolchain \
  --//tools/bazel:firmware_version=<unique-version> \
  //projects/e2e/targets/h2loader_tar_zlib/atomic/devkit:device_test
```

Use `--config=bk7258` and the `atomic/bk7258_v3_202405:device_test` label for BK.
The test invokes H2Loader, never Bazel: it verifies the starting UID and empty
Stage (or resumes only the exact declared package already in Stage), saves P1 and
coredump identities, and transfers the declared package. Serial command services
start after the suite and exact PAL resource comparison, so command polling cannot
change its allocation baseline. Failure starts the recovery channel without
confirming the App. Only actual successful cleanup and confirmation enable an
immutable ledger carrying a cryptographic identity created once per real boot.
A managed run requires a firmware version different from the currently active
App. Rows before the CLI accepts the requested reboot are excluded. The upgrade
and independent App reboot must each provide a complete 56-case ledger with
different execution identities; no replay can stop a command before acceptance. Final checks require the expected
package/image/version, unchanged P1 and empty Stage. Stored coredumps are actually
read and compared byte for byte; an empty dump is checked as unchanged blank
status, with no invented dump or digest. All live test labels use `external`;
build caches remain enabled while test execution is fresh.
