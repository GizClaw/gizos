# SD rename/open SDK audit — 2026-09-17

Closes the O9 item of the [2026-09-14 PAL review](../2026-09-14/pal-review.md). Source is PR #456 (Issue #451), accepted head `5dbfe8c5`, first based on `origin/main` `4fd6e947` and merged with main after #457; the pinned SDK is `eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`. No PAL public header or other platform changed.

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

Packages were built in OrbStack `embed-zig-noble-amd64` with `bazel --output_user_root=/home/idy/.cache/bazel-ac791n/root build --config=ac791n --symlink_prefix=bazel-amd64-` after sourcing the firmware devenv.
The accepted head `5dbfe8c5` produced PAL package `5397f0dcfd88d3d210a6c3411ca38e61e284ab876271035370f65490e745d972` (897111 bytes) with image `31473bc55760834e06817fdb682be34a1f1d37fc56befff10bb1808d8083be52` (908045 bytes).
On UID `d879349abc9f` (`/dev/cu.usbserial-20131240`, 460800) the board started on the main P1 Loader `ed7d71a6…` with `last_result=0`; the package was sent through the UART Loader in 30 s with a matching checksum, independent status showed `stage_valid=1` with that package and image, and `reboot upgrade --monitor` was stopped by SIGINT after 110 s.

Every probe step returned the contract code; the replace and error cases were:

| Probe | Result |
| --- | --- |
| `rename_replace` | rename 0, destination stat 0, source stat -8 |
| `rename_same` | 0 |
| `rename_onto_dir` | -7 |
| `rename_cross_dir` | -3 |
| `rename_open` | rename -18, source still present and destination absent after close |
| `remove_open` | -18 |
| `open_twice` | second write open -18, read open -18 |
| `open_dir_read`, `open_dir_write` | -7, -7 |
| `path_191` | open, close, stat and remove 0 |
| `path_192` | open, stat and remove -13 |
| `component_131` | open, stat and remove -13 |

`utf8_short` (`日志.txt`) and `utf8_long` (`日志-测试-非常长的文件名.txt`) each opened, wrote 4 bytes, closed, reported `is_dir=0 size=4`, read the bytes back and were removed with every step 0.
The raw SDK probes showed `frename` onto an existing name returning -1 with create, close and both `fdelete` steps 0, and `fopen("w+")` of an existing directory returning a handle that closed with 0.
An earlier run of `c72b4e33` (PAL package `05cc543b…`) established the truncation fact: a 160-byte component opened and closed with 0 but stat and remove of the same path returned -8, which is why the head rejects components over 130 UTF-16 units.
After the probes the suites reported `suite=64 case=13 result=0`, Core cases 1, 2, 3, 4, 5, 6, 10 and 11 all 0, `suite=32 case=27 result=0` and `H2_PAL_E2E result=0 passed=10 failed=0`.
Independent status in the App showed `active_role=app`, `running_partition=2`, image `31473bc5…`, `last_result=0`; after `reboot loader` and `stage abort` it showed `active_role=loader`, `running_partition=1`, `active_checksum=ed7d71a6…`, `stage_valid=0`, `last_result=0`.

## Boundaries

The replace is delete-then-rename; a power loss between the two steps leaves no destination, which every in-repo caller already tolerates because it removes the destination itself. Direct SDK callers bypass the registry and the component check. The 130-unit component limit is the SDK's number; the provider enforces it by counting UTF-16 units of the UTF-8 component, so a name the SDK would accept only after truncation is refused up front.
