# Watchdog recovery follow-up

The retained capture implementation is committed in `8d1ec24b`; reviewer changes were merged in `1565ebb8`. The reviewer physically reset the earlier unresponsive Loader trial before this work resumed. No software recovery or direct flash operation was attempted.

## Pinned SDK findings

SDK revision `eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`, `cpu.a` IR (`wdt.c.o`, `p33.c.o`, `poweroff_reboot.c.o`), establishes two production defects. `wdt_init(0xc)` selects four seconds but also enables interrupt mode. Its watchdog ISR prints registers and loops forever. The weak `wdt_clear` unconditionally feeds P33 register `0x80`, bit 6. Emitted production code calls it from the idle hook and the port-yield loop; a progressing core can therefore mask a stopped peer. The earlier trial did request direct reset at one millisecond, so it did not deliberately disable the watchdog. These feed/reset policies predate the retained-block changes; no pre-O10 image was flashed in this follow-up.

`CORE_SYSTEM_RESET` sets bit 4 at `0x10000` and does not disable the P33 watchdog. The production boot patch now always initializes the four-second watchdog and calls `wdt_reset_enable`, clearing interrupt mode. A strong internal-RAM `wdt_clear` consumes progress from both cores before each feed. Its byte admission lock makes one attempt; it never spins waiting for a stopped peer. The underlying RAM P33 primitive serializes the SDK bus and has no callbacks. This detects missing core progress, not every possible task deadlock.

The diagnostic App and Loader hooks now leave watchdog configuration untouched. They report its control byte, disable interrupts on their current core and idle before trial confirmation. Thus the trials exercise the production policy rather than a separately configured diagnostic watchdog.

## Host and native evidence

`//tools/bazel:jieli_watchdog_test` extracts the real board implementation and boot-patch statements. Stopped-core-0, stopped-core-1, healthy alternating progress, boot reset mode and real pthread progress scenarios all fail against the pinned weak feed behavior and old boot patch, then pass with strict Clang/TSan and Linux GCC (`-Wall -Wextra -Werror`). The baseline weak body is reconstructed from the two pinned IR tail calls because the board previously had no override. Existing retained-capture concurrency tests also pass with TSan.

Logs: `/tmp/jieli-watchdog-before.log`, `/tmp/jieli-watchdog-gcc-before.log`, `/tmp/jieli-watchdog-after.log`, `/tmp/jieli-watchdog-gcc-after.log`. Native Loader, PAL and both trial packages build successfully (`/tmp/jieli-watchdog-native.log`). Emitted `wdt_clear` is in internal RAM at `0x01c012cc`; the retained aggregate remains at `0x01c7dd4c`. Host-only iOS analysis passes (`/tmp/jieli-watchdog-ios.log`).

## Board acceptance

The production package `49fc66cf98df5f7eb2daddb79692d2821c003c876d3b0954699ab2b1fd165c91` installed through the Loader's own upgrade flow. UART verified P1 image `635662ded9c2bc0e687038139cbd0af3bc22df212bbd0e86ce7bb5b9a9998e17`, UID `d879349abc9f`, next partition 1 and result 0. Trial results are recorded separately below after both repetitions complete.

Raw artifacts are under `tmp/jieli/pal-review-next/watchdog-rootcause/`: `loader-before.status`, `loader-send.log`, `loader-upgrade.log`, `loader-after.status`, and each trial directory's `uart.raw`, `monitor.log`, `after.status`, `coredump.bin`, `decoded.log` and `result.json`. Raw capture uses the previously instrumented host reader; fixing the ordinary monitor omission remains a separate required item.

The [structured results](./pal-watchdog-recovery.json) record one successful App trial (core 0, status after 45.93 seconds) and one successful Loader trial (core 1, status after 32.71 seconds), measured from the upgrade command including unpack/install. Both reported watchdog control `0x1c`, returned to the expected P1 without human action, and produced checksum-valid 2,096-byte coredumps with reset reason `0x4`. Only the Loader trial enabled Loader-origin recovery. These observations support the paired-core feed correction, but do not complete the required two repetitions.

**Stopped before the second repetitions.** Following the first Loader trial, the ordinary `reboot loader` command exceeded 60 seconds. Its saved `return-loader.log` shows the accepted command, core reset, and repeated Loader-ready messages through 56 seconds; this was not another silent blackout. The host command did not finish, and no subsequent status was attempted. The script terminated its own host reader and stopped, without recovery, another port or direct flash access. Exact current command responsiveness is unverified. The last verified identity is the P1 hash above; P2 and Stage contain the Loader trial candidate `51bde9e23b96681dbe8d0c6a44aaaf6a010d228212407217c99a1e48cca9d09b` (package `9a5fbb4ab72bd94702137c55120138b418e381061f8809bf7ec53402befce6ca`), next partition 1, trial rollback result `-7`.

The watchdog follow-up remains uncommitted because the requested two App and two Loader trials have not passed. Monitor repair, O4 decision record, O11 implementation and final PAL/UART/BLE/audio acceptance remain pending.
