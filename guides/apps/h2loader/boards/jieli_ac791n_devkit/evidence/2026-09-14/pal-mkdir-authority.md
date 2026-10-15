# Filesystem 13 authoritative directory lookup — 2026-09-14

The two preceding current-source PAL runs returned `-7` from Filesystem 13 although a fresh lookup verified the created directory. `create_directory_component` let the trailing-slash creation handle's missing DIR attribute override that lookup.

The post-create lookup now determines the result: directory → OK; regular file → INVALID_STATE; missing entry → IO; failed attribute/close lookup → IO. Creation-handle attribute or close errors do not override a verified directory. Single-level creation and parent checks are unchanged. No flags or compatibility mode were added.

## Host and native evidence

`//tools/bazel:jieli_sd_directory_test` extracts the actual directory/stat implementation. The fake now separates creation-handle metadata from the stored entry, covering missing DIR on the handle plus a directory lookup, missing DIR plus a regular-file lookup, creation-handle attribute failure plus a directory lookup, and creation-handle close failure plus a directory lookup. Missing-entry/error cases, existing directories/files, missing or non-directory parents, concurrent creation, long names and stat-close failures remain covered.

Before repair, strict Clang and GCC abort on the handle-not-directory plus lookup-directory scenario (`/tmp/jieli-mkdir-authority-before.log`, `/tmp/jieli-mkdir-authority-gcc-before.log`). After repair both fixture tests pass with `-Wall -Wextra -Werror` (`/tmp/jieli-mkdir-authority-after.log`, `/tmp/jieli-mkdir-authority-gcc-after.log`). This is a deterministic metadata-authority fix, not a new concurrency claim.

Native Loader, PAL and display packages build successfully in 66.103 seconds (`/tmp/jieli-mkdir-authority-native.log`). The reviewer merge is `b090f2b9`, retaining both local changes and host-only compatibility guards. The requested `ios_sim_arm64 --nobuild --keep_going` analysis of `//tools/bazel:all` and `//native_component_src/jieli/wl82/h2_pal_core:all` passes (`/tmp/jieli-ios-host-guards.log`).

## Pinned SDK explanation

SDK `eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`, archive `cpu/wl82/liba/fs.a` SHA-256 `5c688406ce48dc8f5ec8b87d446b87beda37c7bc5d75a9e6c415d23e267ad11f`:

- `fat_compact.c.o:__fat_fopen` maps `w+` to native mode 12 and calls `jl_f_Open`. `__fat_fget_attr` reads the cached `FIL.dinfo.obj.attrib`; it does not look up the path again.
- `ff_opr.c.o:f_Open_lfn` treats a trailing-slash component as a directory and calls `f_mkdir(..., fp)`. Successful creation continues the parser; an exhausted directory path does not force a fresh read-only open.
- `tff.c.o:f_mkdir` copies `follow_path`'s `DIRINFO` into `fp` **before** creating the missing entry. The creation path writes `(mode & 2) | 16` into the on-disk FAT directory attribute byte and syncs the filesystem. It updates the handle's directory cursor/cluster but does not refresh the cached attribute field after creation.

This explains how the created directory can be correct while the creation handle's attributes do not prove it. A fresh read-only reopen obtains the actual entry metadata. No particular uninitialized attribute value is assumed, and this does not claim arbitrary SDK handle errors are harmless.

IR was inspected in the Linux VM under `/tmp/jieli-fs-inspect.YUeOhV/`. The audited `vfs.c.o`, `fat_compact.c.o`, `ff_opr.c.o` and `tff.c.o` bytes were checked against members of the pinned archive, including its duplicate member names (`/tmp/jieli-mkdir-sdk-objects.log`). Local audit notes and the verification script are in `tmp/jieli/pal-review-next/mkdir-authority/`.

## Hardware

**All three untraced public PAL runs pass 10/10: Filesystem 13, Core cases 1–6/10/11, and offline Wi-Fi 27.** See [acceptance facts and returned status](pal-mkdir-authority.md#retained-acceptance-facts-pal-mkdir-authority-hardware).

Each run installs the same fixed PAL package through UART Loader into P2. Between runs, the normal display App is installed through the same flow, so the next PAL installation is not rejected by the same-image rollback guard. No test instrumentation, tracing, timeout or retry changes were introduced. Core suites completed in 11.596, 15.678 and 10.704 seconds after their entry marker; all produced the successful aggregate.

PAL package SHA-256: `e19f958f1c3667c1e9c06dbb05cd56b4f3f3fe2c1613a079f9fe0e64b6c23194`; image SHA-256: `b893055693de9c7e7430d1dc1acfa93ca5fe03515e09c33aa707b32ab4279375`. The interlude display package is `fa77b14b7254b67cb6e679d7009fcb1969281f4c5eab302504f40aaba15f8fae`, image `f4a1b724edc5b399f5658efaa57d154ef18ee310ee958b0e8bdd1b37d5a09331`.

Final checkpoint state: UID `d879349abc9f`, active valid Loader P1 `2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0`, fixed PAL App P2 and retained staging (`stage_valid=1`), boot intent Loader. P1 was unchanged during these runs. No recovery flashing, format or other-port access occurred. The final-source Loader self-install and full UART/BLE round will be repeated after the remaining items.

Raw evidence is under `tmp/jieli/pal-review-next/diagnostic-runs/mkdir-authority-{1,2,3}/1/` (`pal.log`, `send.log`, `returned.status`) and `mkdir-alternate-{2,3}/`. Frozen packages and SDK audit notes are under `tmp/jieli/pal-review-next/mkdir-authority/`.

## Retained acceptance facts: pal-mkdir-authority-hardware

Historical run summary transcribed from `pal-mkdir-authority-hardware.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| base_commit | `b090f2b9` |
| sd_fs_source_sha256 | `4e29d564b409362dece3342e10f368b81d5252bc0b1b8500a39d5dd20dd47a4c` |
| runs[1].case_count | `10` |
| runs[1].case_results | `0=10` |
| runs[1].variant | `mkdir-authority-1` |
| runs[1].iteration | `1` |
| runs[1].outcome | `aggregate` |
| runs[1].monitor_rc | `130` |
| runs[1].elapsed_s | `34.72017370801768` |
| runs[1].core_elapsed_s | `11.59597833402222` |
| runs[1].package_sha256 | `e19f958f1c3667c1e9c06dbb05cd56b4f3f3fe2c1613a079f9fe0e64b6c23194` |
| runs[1].image_sha256 | `b893055693de9c7e7430d1dc1acfa93ca5fe03515e09c33aa707b32ab4279375` |
| runs[1].core_pass | `True` |
| runs[1].aggregate[1][1] | `0` |
| runs[1].aggregate[1][2] | `10` |
| runs[1].aggregate[1][3] | `0` |
| runs[1].returned_status.device_uid | `d879349abc9f` |
| runs[1].returned_status.active_role | `loader` |
| runs[1].returned_status.active_checksum | `2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0` |
| runs[1].returned_status.active_image_size | `933705` |
| identity.stage_package_checksum | `e19f958f1c3667c1e9c06dbb05cd56b4f3f3fe2c1613a079f9fe0e64b6c23194` |
| identity.stage_image_checksum | `b893055693de9c7e7430d1dc1acfa93ca5fe03515e09c33aa707b32ab4279375` |
| identity.partition_1_package_checksum | `d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909` |
| identity.partition_1_image_checksum | `2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0` |
| identity.partition_2_package_checksum | `e19f958f1c3667c1e9c06dbb05cd56b4f3f3fe2c1613a079f9fe0e64b6c23194` |
| identity.partition_2_image_checksum | `b893055693de9c7e7430d1dc1acfa93ca5fe03515e09c33aa707b32ab4279375` |
| runs[2].case_count | `10` |
| runs[2].case_results | `0=10` |
| runs[2].variant | `mkdir-authority-2` |
| runs[2].iteration | `1` |
| runs[2].outcome | `aggregate` |
| runs[2].monitor_rc | `130` |
| runs[2].elapsed_s | `38.48210220798501` |
| runs[2].core_elapsed_s | `15.677648667013273` |
| runs[2].package_sha256 | `e19f958f1c3667c1e9c06dbb05cd56b4f3f3fe2c1613a079f9fe0e64b6c23194` |
| runs[2].image_sha256 | `b893055693de9c7e7430d1dc1acfa93ca5fe03515e09c33aa707b32ab4279375` |
| runs[2].core_pass | `True` |
| runs[2].aggregate[1][1] | `0` |
| runs[2].aggregate[1][2] | `10` |
| runs[2].aggregate[1][3] | `0` |
| runs[2].returned_status.device_uid | `d879349abc9f` |
| runs[2].returned_status.active_role | `loader` |
| runs[2].returned_status.active_checksum | `2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0` |
| runs[2].returned_status.active_image_size | `933705` |
| runs[3].case_count | `10` |
| runs[3].case_results | `0=10` |
| runs[3].variant | `mkdir-authority-3` |
| runs[3].iteration | `1` |
| runs[3].outcome | `aggregate` |
| runs[3].monitor_rc | `130` |
| runs[3].elapsed_s | `33.60711920799804` |
| runs[3].core_elapsed_s | `10.704230999981519` |
| runs[3].package_sha256 | `e19f958f1c3667c1e9c06dbb05cd56b4f3f3fe2c1613a079f9fe0e64b6c23194` |
| runs[3].image_sha256 | `b893055693de9c7e7430d1dc1acfa93ca5fe03515e09c33aa707b32ab4279375` |
| runs[3].core_pass | `True` |
| runs[3].aggregate[1][1] | `0` |
| runs[3].aggregate[1][2] | `10` |
| runs[3].aggregate[1][3] | `0` |
| runs[3].returned_status.device_uid | `d879349abc9f` |
| runs[3].returned_status.active_role | `loader` |
| runs[3].returned_status.active_checksum | `2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0` |
| runs[3].returned_status.active_image_size | `933705` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record.runs[1].returned_status, record.runs[2].returned_status, record.runs[3].returned_status | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=1; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=b893055693de9c7e7430d1dc1acfa93ca5fe03515e09c33aa707b32ab4279375; partition_2_package_checksum=e19f958f1c3667c1e9c06dbb05cd56b4f3f3fe2c1613a079f9fe0e64b6c23194; stage_image_checksum=b893055693de9c7e7430d1dc1acfa93ca5fe03515e09c33aa707b32ab4279375; stage_package_checksum=e19f958f1c3667c1e9c06dbb05cd56b4f3f3fe2c1613a079f9fe0e64b6c23194` |
