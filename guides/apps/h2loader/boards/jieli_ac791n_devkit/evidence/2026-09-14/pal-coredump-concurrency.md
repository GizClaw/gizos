# Coredump concurrency and watchdog origin

The O10 implementation committed in `8d1ec24b` uses separate single-attempt byte locks for the retained log and pending record. An assert never enters the normal atomic runtime's blocking testset lock. If another core owns the log, capture emits a valid metadata-only record instead of copying potentially torn bytes. If another capture owns the record, the competing capture returns without modifying it. Log producers likewise drop a byte on contention rather than blocking an exception path. Flash writes and readback run outside both locks; a newer pending record is retained if it arrives during persistence.

Each ring write publishes a dirty magic before changing data/head/total and clears it after a barrier. Reset recovery rejects dirty rings. The same existing magic word carries the previous image role; early boot consumes that role before publishing the new image's role. Logging stays disabled until that recovery hook completes, so early SDK output cannot overwrite the old role. The Loader component supplies its image marker; Apps have no Loader marker. Recovery mode now follows a valid retained Loader-origin record, rather than treating every watchdog reset as a Loader crash. The 2,096-byte v2 flash record remains unchanged.

## SDK and emitted-code evidence

Pinned SDK: `eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`. `include_lib/driver/cpu/wl82/asm/cpu.h` uses `testset b[...]` and branches on equal to retry a busy lock. The new SDK-port helper makes exactly one attempt. Native disassembly confirms `csync`, clear result, one `testset`, a forward conditional branch, set result on success, `csync`, return. It resides in internal RAM; no compiler-rt synchronization routine is called by this helper.

`apps/common/system/init.c` calls `setup_arch()` before `os_init()` and scheduler startup. The existing warm-boot hook runs before `setup_arch()`. The existing reset hook runs inside `setup_arch()`. These boot phases permit recovery before task log producers start; subsequent log/capture operations use the separate admission locks. The watchdog test component uses the SDK's documented `WDT_1MS` setting and direct reset mode, before trial confirmation.

Native LTO exposed an additional defect: the previous retained ring was split into `retained_log.0` through `.3`, with head/total stored before magic. Declared C field order was not the emitted layout. Separate retained globals therefore did not establish a portable Loader/App RAM ABI. The fix keeps one externally retained, used 4,168-byte aggregate in a dedicated linker input section. Loader and PAL App symbol tables both place it at `0x01c7dd4c`; the log begins at offset 2,108 (`0x01c7e588`), and the SDK boot marker follows at `0x01c7ed94`. Compile-time bounds check the size and log offset. The warm snapshot reader uses these new offsets and rejects dirty/invalid log magic.

This deliberately replaces the old unstable retained layout. Old in-RAM breadcrumbs are not preserved across this transition; flash v2 records remain readable. Install the new Loader through its own upgrade flow before using new-layout Apps. The warm request and handoff addresses are unchanged, and the aggregate plus SDK marker ends below the existing warm snapshot at `0x01c7ee00`.

## Regression checks

`//tools/bazel:jieli_coredump_concurrency_test` compiles the real coredump source, Loader recovery expression and warm report against host SDK fakes. Fifteen scenarios run for both image roles: origin recovery, early role, invalid pending records, recovery policy, pre-recovery logging, capture during flash persistence, saturated log count, dirty rings, held log/capture locks, real concurrent writers/captures, a deterministically paused writer, and warm snapshot validity/layout. Against `5ee52a92`, 25 cases fail with Clang/TSan and 23 fail with Linux GCC; both concurrent-role cases additionally report real TSan races before the fix. All 30 pass after the fix with strict `-Wall -Wextra -Werror` on both compilers and with TSan.

`//tools/bazel:jieli_decode_coredump_test` adds acceptance of Loader-role ring magic and rejection of dirty magic; that added case fails before and passes after. The fixture routes the SDK formatter declaration through a host `vsnprintf` fake, avoiding macOS fortified-libc macro substitution while preserving formatted output.

Logs: `/tmp/jieli-coredump-before.log`, `/tmp/jieli-coredump-gcc-before.log`, `/tmp/jieli-coredump-after.log`, `/tmp/jieli-coredump-gcc-after.log`. Native Loader, PAL, Display and both watchdog trial package targets build successfully; the final build invocation completed in 34.090 seconds with prior successful package actions cached. iOS host exclusion analysis and the guides build pass. Contention deliberately loses log bytes; this provides a coherent available snapshot, not lossless exception logging.

## Hardware blocker: stop after Loader watchdog trial

The [hardware record](pal-coredump-concurrency.md#retained-acceptance-facts-pal-coredump-hardware) contains exact package and image hashes. The new production Loader was installed through the existing Loader upgrade flow and verified as P1 image `63157615c4ab3d2a3fe4168e955cd7a640a4662bc4fbbec46e789646c5f09cd1` (package `9e774c5cba488053b069d3d8e66fe11fdafa534e76c3cf672b5cef5783337252`).

The App watchdog trial returned to that Loader. UART status and a checksum-valid 2,096-byte coredump passed; reset reason is `0x4` (WDT), and no Loader crash-recovery marker appeared. The host monitor exceeded its existing 15-second SIGINT shutdown wait but exited before reader inspection; this harness failure is recorded separately from the subsequent successful board checks. The dump's boot stage is 80, because the SDK boot probe overwrites that marker before reset recovery; it is not evidence of the faulting App's last stage.

The Loader watchdog trial then staged successfully. The Loader reported successful header capture and `H2_JIELI_LOADER_COMMIT mode=warm boot_info=unpublished`. Its final captured line was `H2_JIELI_REBOOT_EXECUTE reset=core`. The subsequent UART status call timed out with code `-6`; no post-trial identity or coredump could be obtained. Board work stopped without a recovery attempt, another-port access, USB DL or formatting. The cause is not established, and the current board boot state is unverified. P1's hash above is the last verified state, not a claim about the current state.

Raw evidence: `tmp/jieli/pal-review-next/o10/loader-before.status`, `loader-upgrade.log`, `loader-after.status`, `app-watchdog/watchdog.log`, `app-watchdog/after.status`, `app-watchdog/coredump.bin`, `app-watchdog/decoded.log`, `app-watchdog/monitor-shutdown.status`, `loader-watchdog/watchdog.log`, and `loader-watchdog/after.status`. Emitted-code evidence is in `testset.dis`, `loader-retained.symbols`, and `pal-retained.symbols` in that same directory. O10 remains open; its implementation and diagnostic targets are uncommitted. Final PAL, UART lifecycle and BLE lifecycle runs have not been performed on this source.

## Subsequent investigation

See the [watchdog recovery follow-up](./pal-watchdog-recovery.md) for the SDK policy defects, production correction and partial repeat-trial results. The historical blackout above was physically reset by the maintainer before that investigation.

## Retained acceptance facts: pal-coredump-hardware

Historical run summary transcribed from `pal-coredump-hardware.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| source | `uncommitted O10 work based on 5ee52a92` |
| loader_install.source | `O10 working source after 5ee52a92` |
| loader_install.package_sha256 | `9e774c5cba488053b069d3d8e66fe11fdafa534e76c3cf672b5cef5783337252` |
| loader_install.image_sha256 | `63157615c4ab3d2a3fe4168e955cd7a640a4662bc4fbbec46e789646c5f09cd1` |
| loader_install.status.device_uid | `d879349abc9f` |
| loader_install.status.active_role | `loader` |
| loader_install.status.active_checksum | `63157615c4ab3d2a3fe4168e955cd7a640a4662bc4fbbec46e789646c5f09cd1` |
| loader_install.status.active_image_size | `936169` |
| identity.partition_1_package_checksum | `9e774c5cba488053b069d3d8e66fe11fdafa534e76c3cf672b5cef5783337252` |
| identity.partition_1_image_checksum | `63157615c4ab3d2a3fe4168e955cd7a640a4662bc4fbbec46e789646c5f09cd1` |
| identity.partition_2_package_checksum | `9e774c5cba488053b069d3d8e66fe11fdafa534e76c3cf672b5cef5783337252`; `a6b4f759004630c2a2508daee3cf7d822af778d4d297c8fecd2ac2afafb69cc5` |
| identity.partition_2_image_checksum | `63157615c4ab3d2a3fe4168e955cd7a640a4662bc4fbbec46e789646c5f09cd1`; `19eaef2d797d6c2bfd314746f72a7faabf2361c8f5859d83dbaae162b18dc31c` |
| app_watchdog.trial | `app-watchdog` |
| app_watchdog.package_sha256 | `a6b4f759004630c2a2508daee3cf7d822af778d4d297c8fecd2ac2afafb69cc5` |
| app_watchdog.p1_sha256 | `63157615c4ab3d2a3fe4168e955cd7a640a4662bc4fbbec46e789646c5f09cd1` |
| app_watchdog.reset_reason | `4` |
| app_watchdog.boot_stage | `80` |
| app_watchdog.log_bytes | `2048` |
| app_watchdog.recovery_marker | `False` |
| app_watchdog.status.device_uid | `d879349abc9f` |
| app_watchdog.status.active_role | `loader` |
| app_watchdog.status.active_checksum | `63157615c4ab3d2a3fe4168e955cd7a640a4662bc4fbbec46e789646c5f09cd1` |
| app_watchdog.status.active_image_size | `936169` |
| identity.stage_package_checksum | `a6b4f759004630c2a2508daee3cf7d822af778d4d297c8fecd2ac2afafb69cc5` |
| identity.stage_image_checksum | `19eaef2d797d6c2bfd314746f72a7faabf2361c8f5859d83dbaae162b18dc31c` |
| app_watchdog.monitor_shutdown | `SIGINT wait exceeded 15 seconds; process exited before lsof inspection; subsequent status/dump passed` |
| loader_watchdog.trial | `loader-watchdog` |
| loader_watchdog.result | `blocked` |
| loader_watchdog.package_sha256 | `c3feec8f6179578411b9f400bdebdf4ce005c5a2a443ed15d91c2d9373cca1f8` |
| loader_watchdog.image_sha256 | `3fca330cb29cc8f39e8121f5b8a2c5b9da943544e4d60811bd6ef139434bd646` |
| loader_watchdog.last_verified_p1_sha256 | `63157615c4ab3d2a3fe4168e955cd7a640a4662bc4fbbec46e789646c5f09cd1` |
| loader_watchdog.status_error | `-6` |
| loader_watchdog.current_board_state | `unverified after watchdog trial reboot handoff` |
| loader_watchdog.recovery_attempted | `False` |
| loader_watchdog.last_monitor_line | `H2_JIELI_REBOOT_EXECUTE reset=core` |
| loader_watchdog.p2_header_before_handoff | `SDK completion returned zero; Loader reported boot_info=unpublished` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record.loader_install | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=63157615c4ab3d2a3fe4168e955cd7a640a4662bc4fbbec46e789646c5f09cd1; partition_1_package_checksum=9e774c5cba488053b069d3d8e66fe11fdafa534e76c3cf672b5cef5783337252; partition_2_image_checksum=63157615c4ab3d2a3fe4168e955cd7a640a4662bc4fbbec46e789646c5f09cd1; partition_2_package_checksum=9e774c5cba488053b069d3d8e66fe11fdafa534e76c3cf672b5cef5783337252` |
| record.app_watchdog | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=63157615c4ab3d2a3fe4168e955cd7a640a4662bc4fbbec46e789646c5f09cd1; partition_1_package_checksum=9e774c5cba488053b069d3d8e66fe11fdafa534e76c3cf672b5cef5783337252; partition_2_image_checksum=19eaef2d797d6c2bfd314746f72a7faabf2361c8f5859d83dbaae162b18dc31c; partition_2_package_checksum=a6b4f759004630c2a2508daee3cf7d822af778d4d297c8fecd2ac2afafb69cc5; stage_image_checksum=19eaef2d797d6c2bfd314746f72a7faabf2361c8f5859d83dbaae162b18dc31c; stage_package_checksum=a6b4f759004630c2a2508daee3cf7d822af778d4d297c8fecd2ac2afafb69cc5` |
