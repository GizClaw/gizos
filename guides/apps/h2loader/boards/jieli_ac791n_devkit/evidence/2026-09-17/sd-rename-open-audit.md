# SD rename/open SDK audit — 2026-09-17

Closes the O9 item of the [2026-09-14 PAL review](../2026-09-14/pal-review.md). Source is PR #456 (Issue #451) on top of `origin/main` `4fd6e947`; the pinned SDK is `eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`. No PAL public header or other platform changed.

## SDK facts

`fs.a` ships as LLVM bitcode; the functions below were read from IR produced by the pinned toolchain's `clang -x ir -S -emit-llvm` after `ar x` (GNU `ar xN 2` for the duplicate `tff.c.o` member). The `jlfat` operation table `jl_fat_vfs_ops` lives in the first `fat_compact.c.o` member and routes `fopen` through `ff_opr.c.o:jl_f_Open` → `f_Open_lfn` → `tff.c.o:f_open`.
`ff_opr.c.o:f_Rename` returns -1 when the new name contains `/` (except the SDK's `\U` unicode marker) and when `follow_path` finds the destination already present, logging `file name is existing`; `tff.c.o:f_ReName` then clears the old entries and registers new ones at a new directory position, so only the renaming handle keeps a valid entry pointer.
`vfs.c.o:fdelete` calls `__fat_fdelete` → `f_unlink` and always `fclose`s the handle afterwards, so a failed unlink still consumes the handle.
`tff.c.o:f_open` rejects a found entry with `AM_RDO|AM_DIR` (`0x11`) for write modes with error 136, which `fopen` reports as NULL, while read mode opens a directory (the SDK's `fdir_exist` depends on it) and `f_read` then returns directory sectors.
`vfs.c.o:f_free_cache` calls `ops->ioctl(part, 3, &err)` and returns `err`; `__fat_ioctl` has no case 3 (its switch covers 1 and 5–22), so on jlfat `f_free_cache` succeeds without flushing and data reaches the card in `__fat_fclose` → `f_sync_fs`.
`f_Open_lfn` copies the path into a 256-byte `path_c` and 260-byte name buffers, `f_PickOutName` bounds one component at 130 UTF-16 units, base names longer than 8 or extensions longer than 3 go through `utf8_2_unicode`, and `f_short_name_deal` builds a unicode long name for short names with non-8.3 bytes.

## Provider changes

`fs_rename` deletes an existing regular-file destination through `fdelete` and then renames (documented as non-atomic), returns OK for identical mapped paths after an existence check, `H2_PAL_ERR_INVALID_STATE` for a directory destination, keeps `H2_PAL_ERR_UNSUPPORTED` across directories, and propagates the read handle's close result.
`fs_open` rejects directories in both modes with `H2_PAL_ERR_INVALID_STATE`; a truncating open first inspects an existing entry read-only because, as the board probe showed, jlfat's `fopen("w+")` on a directory returns a handle.
`translate_path` rejects any component longer than 130 UTF-16 units with `H2_PAL_ERR_NO_SPACE`, because the board probe showed the SDK silently creating a 160-byte name under a truncated name that the original path could no longer find.
An open-handle registry keyed by mapped path and mode, guarded by an atomic gate held across each native mutation, returns `H2_PAL_ERR_BUSY` for rename, remove, clear (including descendants) and truncating opens of a held path and for a read open while a writer holds it; two readers may coexist.

## Host validation

`bazel test --config=macos_arm64 //tools/bazel:all` passes 99/99 with the new `jieli_sd_rename_test` and `jieli_sd_open_handles_test` and the extended `jieli_sd_paths_test` (130-unit ASCII, CJK and emoji component boundaries).
Against `main` `4fd6e947` the rename fixture fails at the replace assertion (the provider returned IO), the handle fixture fails at the first BUSY assertion (the provider opened a second writer), and the path fixture fails at the 131-byte component rejection.
The threaded handle fixture passes under macOS `-fsanitize=thread` and Linux GCC in OrbStack; `bazel build --config=ios_sim_arm64 --nobuild --keep_going //tools/bazel:all` passes.

## Native build and board run

The Loader and PAL packages were built in OrbStack `embed-zig-noble-amd64` with `bazel --output_user_root=/home/idy/.cache/bazel-ac791n/root build --config=ac791n --symlink_prefix=bazel-amd64-`; the run below used the PAL package built from `c72b4e33` (`05cc543b805bc585f7f68c91c1606dd20b284e6b79d629095a5d2ae3fdc6992a`, image `9e9732c9263d2c95f40b6251a38aab1aaaef944f5f7c2fe07645a4abff2ab0fc`), and the package from `41b6957f` (`862ddeaff9b8841e07b65e2560ad7612fae6b130f5aa77cd3acd21aed1051557`, image `6fe08af9fe700307b1f0151b493a825bef512ecab2d8d37c6eac6cf879ef869d`) adds only the component-limit rejection.
On UID `d879349abc9f` (`/dev/cu.usbserial-20131240`, 460800) the board started on the main P1 Loader `ed7d71a6…` with `last_result=0`; the package was sent through the UART Loader in 29 s with a matching checksum, and `reboot upgrade --monitor` was stopped by SIGINT after 110 s.
Probe results with PAL codes: `rename_replace` create a, create b, rename 0, then `stat_b` 0 and `stat_a` -8; `rename_same` 0; `rename_onto_dir` -7; `rename_cross_dir` -3; `rename_open` rename -18 with a still present and b absent after close; `remove_open` -18; `open_twice` second write open -18 and read open -18; `open_dir_read` -7 and `open_dir_write` -7; `path_191` open, close, stat and remove all 0; `path_192` open -13, stat -13, remove -13.
`utf8_short` (`日志.txt`) and `utf8_long` (`日志-测试-非常长的文件名.txt`) each opened, wrote 4 bytes, closed, reported `is_dir=0 size=4`, read the bytes back and were removed with every step 0.
`component_160` exposed the SDK truncation: open and close returned 0 but stat and remove returned -8, so the entry existed under a different name; `41b6957f` now rejects such components with -13 before the SDK sees them (host-verified; the board re-run of the renamed `component_131` probe is recorded below when the board is next available).
Raw SDK probes: `frename` onto an existing name returned -1 (`sdk_frename_existing rename native=-1`) while the create, close and both `fdelete` steps returned 0, and `fopen("w+")` of an existing directory returned a handle (`sdk_fopen_dir_write open native=1`, close 0).
After the probes the suites reported `suite=64 case=13 result=0`, Core cases 1, 2, 3, 4, 5, 6, 10 and 11 all 0, `suite=32 case=27 result=0` and `H2_PAL_E2E result=0 passed=10 failed=0`; independent status showed `active_role=app`, `running_partition=2`, image `9e9732c9…`, `last_result=0`, and after `reboot loader` the status showed `active_role=loader`, `running_partition=1`, `active_checksum=ed7d71a6…`, `last_result=0`.

## Boundaries

The replace is delete-then-rename; a power loss between the two steps leaves no destination, which every in-repo caller already tolerates because it removes the destination itself. Direct SDK callers bypass the registry. The 130-unit component limit is the SDK's; the provider still enforces only the 191-byte translated path limit.
