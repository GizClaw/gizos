# USB recovery and PAL directory failure

Base revision: `7b7b714639b0d0cafd7ae2bddcba6b21d26106b1`.

USB `4c4a:8057`, location `20131230`, was passed to OrbStack. The official
downloader identified Flash UUID `75 C7 79 16 09 13 62 53 FF FF FF FF FF FF FF FF`
and 16 MB capacity. `-format all` reported erase `[0x0-0xfff000]` SUCCESS,
Download completed, and reboot. This is the vendor's reported erase range,
not a claim that the final reserved 4 KiB was erased.

Factory inputs reconstructed from the current ELF produced byte-identical
Bazel factory and managed images before flashing:

- Factory SHA256: `ec1c6e3066ceac39a09a0480f052335a7007c1f477b8f1867ea27461b53c8528`.
- Managed image SHA256: `b1eae8dceeeef170f853e32ac87641b9f01085aa9c03bb902cf21ae5959cc0df`.
- ELF SHA256: `8256cf7518730270b8cb809085709a7b4e49b992b48ffb18de43ecfa676ebc55`.

After flashing, UART 460800 status confirmed P1 Loader valid/running,
P2 invalid, no Stage and last_result=0. New UID: `d879349abc9f`.
Initial monitoring missed startup; later command traffic captured real UART
logs. The normal buffered printf output of the PAL App was not observable.
The diagnostic target now emits progress via the existing atomic board console
and wraps filesystem calls to identify their failing operation.

The current public PAL package uploaded, installed and booted into P2 through
UART Loader. Diagnostic builds reported nine passed and one failed:
Core cases 1,2,3,4,5,6,10,11 and Wi-Fi case 27 passed; Filesystem case 13
failed with IO (-4) at mkdir(`/data/pal-host-e2e`), before file reads/writes.
One intermediate run stopped reporting after Core suite entry while UART
status and reboot-loader remained functional; later runs completed Core.
That intermittent behavior is not explained by a later passing run.

A parent/basename fmk_dir experiment did not fix the failure: SDK returned
132 and subsequent lookup returned NOT_FOUND. Short-name probes returned
native 0/133 but still NOT_FOUND at the requested nested paths. Their meaning
and the correct directory creation/lookup contract remain unverified. The PAL
behavior experiment and its proposed regression fixture were removed; only
diagnostic target changes remain. No unverified filesystem fix was pushed.

Earlier recovery status (not the latest diagnostic run): P1 Loader active, checksum
`7130cfe2386c86a7dcf82ccd15854f14b64fb525be14a984cfdaa5dd19d64dfa`,
last_result=0. P2 and Stage retain the diagnostic package
`ebb2b6408fb1fe1955577ea2d52c3ca27aa3314681079eb2e0698e2102897864`;
do not mistake it for the PR version. UART is released. No manual Reset was
needed after initial USB recovery. Self-update and full BLE E2E were not
revalidated in this run; this does not close PR merge gates.

Local captures are under `tmp/jieli/current-*`, notably
`current-usb-recovery-flash.log`, `current-pal-fs-first-monitor.log`,
`current-pal-dir-probe-monitor.log`, and `current-recovery-final-status.log`.

### SDK directory API inspection

Inspected LLVM IR from the pinned SDK `cpu/wl82/liba/fs.a:vfs.c.o`.
`fmk_dir` selects the mount and partition using its first argument, then passes
only the second argument (`folder`) to filesystem ioctl 15. It does not append
the first argument's parent-directory suffix. Thus the removed parent/basename
experiment did not preserve nested-path semantics; its short-name success
cannot prove creation at the requested nested path.

`fdir_exist` encodes long names and calls `fopen(..., "r")`, returning existence
without checking `F_ATTR_DIR`. Replacing attribute-aware stat with that helper
would incorrectly accept a regular file. Neither observation yet establishes
why the original full nested path fails to create; no production PAL change
is justified by these observations alone.

The pinned `tff.c.o` debug enum identifies 132 as `FR_NO_PATH`, 133 as
`FR_EXIST`, and 134 as `FR_INVALID_NAME`. A new diagnostic-only package
`075dfd36aebbd35697358abc628338fb676befab1f7c45f2bafe7e3859ad6271`
was uploaded through UART and installed without touching P1. With the original
full-path mkdir implementation, the board reports:

```text
H2_PAL_FS mkdir path=storage/sd0/C/data/pal-host-e2e native=132 lookup=-8
H2_PAL_E2E suite=64 case=13 result=-4
```

This establishes the native failure before file I/O, not its underlying cause.
The new weak diagnostic observer changes no mkdir return behavior and is not
yet intended for release.

Encoding the full folder argument using SDK `long_file_name_encode` was also
tested in package `d188eaa4bd6791e4cd03f0c34fcc36505f38282874dbcdd24d32bf5ca566432a`.
It still returned native 132 and lookup -8 at the same path. That encoding-only
experiment was reverted; it is not a verified fix. P1 remained unchanged.

A trailing-slash `fopen_by_utf8(..., "w+")` experiment in package
`2129c6daa868f128a04c7ac6e6b3e9a787213c13156a8d1006d554d5dab728d0`
created a regular file, not a directory: lookup returned INVALID_STATE (-7).
The public E2E cleanup removed that test path successfully. The experiment was
reverted. Attribute validation correctly prevented this from passing mkdir.

### Diagnostic checkpoint pushed at user request

The current source includes an unfinished, hardcoded short-directory/rename
probe for `/data/pal-host-e2e`, plus direct PAL progress logging. These changes
are a diagnostic checkpoint, not a production fix or merge-ready acceptance.
The earlier statement that no unverified filesystem fix was pushed describes
the earlier recovery only.

Package `881f6f25f36771323ff94288369df02e57510ed7801c1247665b620d51abd4fa`
was installed through UART. Native image write completed with result 0 and
893117 bytes. Both the original directory and `/data/h2md0001` short-directory
probe returned native 132. The rename branch was therefore never exercised.
Filesystem case 13 still returned -4. The last captured App progress was Core
suite entry; the host monitor subsequently exited with command code -7.
This does not prove a board crash or a completed Core suite. No new claim of
self-update, BLE E2E, or overall PAL acceptance is made by this checkpoint.

The hardcoded short-directory/rename probe was then removed from the board
PAL: it never succeeded on hardware and special-cased the E2E fixture path.
`ensure_directory()` again issues a single full-path `fmk_dir` and reports
it through the optional weak `h2_jieli_sd_fs_trace_mkdir` observer without
changing the return value. Filesystem case 13 remains a known failure until
the nested-directory `FR_NO_PATH` cause is understood.


## 2026-09-14 — long-directory parser and duplicate encoding

Starting revision: `0730f9089533515f8f799a925dc0e91bbb5b0eef`.
The port was re-listed and no stale reader held `/dev/cu.usbserial-20131240`.
UART 460800 status identified `d879349abc9f`. Every package below was an App
installed into P2 using UART `send` and `reboot upgrade --monitor`. P1 was not
written or erased; its checksum remained
`7130cfe2386c86a7dcf82ccd15854f14b64fb525be14a984cfdaa5dd19d64dfa`.
No USB downloader, physical recovery, BLE, or Loader self-update was used.

### Parent-directory hypothesis rejected on the board

Package SHA-256:
`4ce210ad86ca721899eaa7493567f53c1f3cf2ac75304f2fc85d7292d12cc056`.
With the original board PAL, the diagnostic target reported:

```text
H2_PAL_FS parent path=/dl stat=0 is_dir=1
H2_PAL_FS parent path=/dl mkdir=0
H2_PAL_FS parent path=/data stat=0 is_dir=1
H2_PAL_FS parent path=/data mkdir=0
H2_PAL_FS mkdir path=storage/sd0/C/data/pal-host-e2e native=132 lookup=-8
H2_PAL_E2E suite=64 case=13 result=-4
```

Thus `/data` existed and was a directory. Creating it again did not fix the
long child name. This run captured only Core suite entry afterwards; it has
no complete Core or Wi-Fi result. UART status and return to Loader succeeded.

A controlled short/long-name comparison used package
`b9f17d9325497401977c43c7f91a6748e341310cd0a7771003c43d06b5731fbb`:

```text
H2_PAL_FS probe path=/dl/h2md0001 native=0 stat=0 is_dir=1
H2_PAL_FS probe path=/data/h2md0001 native=0 stat=0 is_dir=1
H2_PAL_FS probe path=/DATA/H2MD0002 native=0 stat=0 is_dir=1
H2_PAL_FS probe path=/data/h2md0001/child native=0 stat=0 is_dir=1
H2_PAL_FS probe path=/data/long-directory-name native=132 stat=-8 is_dir=0
H2_PAL_E2E suite=64 case=13 result=-4
H2_PAL_E2E result=-4 passed=9 failed=1
```

This fresh run proves short nested creation and component-by-component descent
work on this card. It supersedes neither the capture nor the unexplained
native-132 short-name observation from the earlier checkpoint. In this run,
Core cases 1, 2, 3, 4, 5, 6, 10, 11 and offline Wi-Fi case 27 each reported 0.

### SDK cause and directory creation route

The pinned SDK is `eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`.
Inspection of `fs.a` LLVM IR confirms:

- `fmk_dir` passes the folder to ioctl 15 and then `f_mkdir(..., fp=NULL)`.
  `create_name` stops at eight basename characters. If the resulting lookup
  fails with unconsumed characters, `follow_path` returns 132 (`FR_NO_PATH`).
  This explains the misleading error for the long *leaf*, despite a valid
  parent.
- SDK `fopen` already calls `long_file_name_encode` for JLFAT unless the first
  volume-relative component starts with the encoded-name marker. Calling
  `fopen_by_utf8` first therefore encodes a long child twice when the first
  component is short `/data` or `/dl`; embedded UTF-16 NULs truncate the second
  pass. Changing mkdir alone would leave open/stat/remove using the wrong name.
- The `fopen` long-name parser treats a trailing slash as a directory component.
  The SDK header documents automatic file/directory creation through `fopen`.

Package `c7614511bd2fb026852fd2391bbdb8a621362e0cd920759e21530cc7adab340c`
called raw SDK `fopen("storage/sd0/C/data/h2-raw-directory/", "w+")` for
openprobe index 0. The returned handle had `F_ATTR_DIR` (16), while the old
PAL lookup still failed:

```text
H2_PAL_FS openprobe index=0 opened=1 attr_rc=0 attr=16
H2_PAL_FS openprobe index=0 close_or_delete=0
H2_PAL_FS openprobe path=/data/h2-raw-directory stat=-8 is_dir=0
H2_PAL_E2E suite=64 case=13 result=-4
```

The alternative child-file and short-directory/rename experiments in that
package were diagnostic only and are absent from the final target and PAL.
This run did not capture a complete Core/Wi-Fi ledger. One status attempt
returned host code -2 while its monitor was still alive; after the monitor
exited, an exclusive status succeeded, followed by UART return to Loader.
This is not evidence that the board stopped responding to UART.

### General PAL correction and component proof

All mapped opens now use SDK `fopen` so paths are encoded once. Directory
creation walks each component in order, checks existing FAT attributes, and
opens missing components with a trailing slash. It checks both the created
handle and the requested entry, closes handles, and propagates errors. Existing
directories succeed; regular files return INVALID_STATE; an uncreated entry
returns IO. No fixture names, temporary child files, or rename workaround are
in the board PAL. The existing weak observer now records the create-handle
attribute/close result in `native`, rather than a `fmk_dir` return code.

Package `92e243649a7a0c5d35d1778b797585df3f8fb4658157a93624d9a00e6e46ba6f`
proved creation from a missing parent and idempotence:

```text
H2_PAL_FS mkdir path=storage/sd0/C/data/h2-dir-regression native=0 lookup=0
H2_PAL_FS mkdir path=storage/sd0/C/data/h2-dir-regression/long-parent native=0 lookup=0
H2_PAL_FS mkdir path=storage/sd0/C/data/h2-dir-regression/long-parent/child native=0 lookup=0
H2_PAL_FS nested before=-8 mkdir=0 stat=0 is_dir=1 again=0
H2_PAL_FS cleanup path=/data/h2-dir-regression/long-parent/child result=0
H2_PAL_FS cleanup path=/data/h2-dir-regression/long-parent result=0
```

No completion line arrived for removing the outer diagnostic directory, so
this package did not reach the public suites. UART status and return to Loader
succeeded. The cause of the missing cleanup progress remains unverified; the
extra creation/cleanup probe was removed from the final target. Its component
success must not be described as a complete Filesystem case result. The final
target adds only read-only `/dl` and `/data` attribute logs before public suites.
Exploratory directory entries may remain on the card; no broad cleanup was run.

### Public Filesystem run and validation

Final package SHA-256:
`69e6ee24388225491f4e0d2b7486b9072393792d69dbe1aaa482cc43c39a3132`.
UART send reported 881050 bytes and this exact digest.

Host command:
`bazel test --config=macos_arm64 //tools/bazel:jieli_sd_directory_test`
passed, freshly executing the target (1.7 seconds). The C fixtures cover
ordered missing-parent creation, existing directories/files, component failure
short-circuiting, attribute and close errors, missing post-create entries,
concurrent creation, and the directory-buffer boundary.

Native package build passed in OrbStack `embed-zig-noble-amd64`, using
`--output_user_root=/home/idy/.cache/bazel-ac791n/root`, `--config=ac791n`, and
`--symlink_prefix=bazel-amd64-` after sourcing the specified devenv and unsetting
the four ESP-IDF variables. Final build elapsed time: 35.862 seconds.

Local captures and packages are under `tmp/jieli/2026-09-14-*`; the phase names
are `parent`, `components`, `lfn`, `fixed` (component probe), and `public`.

The final public run captured every case and the aggregate:

```text
H2_PAL_FS mkdir path=storage/sd0/C/data/pal-host-e2e native=0 lookup=0
H2_PAL_FS op=mkdir result=-7
H2_PAL_E2E suite=64 case=13 result=0
H2_PAL_E2E suite=1 case=1 result=0
H2_PAL_E2E suite=1 case=2 result=0
H2_PAL_E2E suite=1 case=3 result=0
H2_PAL_E2E suite=1 case=4 result=0
H2_PAL_E2E suite=1 case=5 result=0
H2_PAL_E2E suite=1 case=6 result=0
H2_PAL_E2E suite=1 case=10 result=0
H2_PAL_E2E suite=1 case=11 result=0
H2_PAL_E2E suite=32 case=27 result=0
H2_PAL_E2E result=0 passed=10 failed=0
```

The mkdir -7 line is the public case's intentional attempt to mkdir the
regular `value` file. The case also passed directory/file stat, exact file
size, write/read contents, EOF, seek, file and directory removal, and path
traversal rejection. The trace includes both successful remove returns.
The monitor subsequently exited with host code -7 after printing the passing
ledger; an exclusive UART status then succeeded. This monitor termination
and the earlier incomplete runs are not explained by the passing suite.

Readback status identified the final package in P2 with image SHA-256
`6be8c307f7c1a2ec03d3cecf58ea53907f4acc7b102bb40d5ed6cbe4a8f37d2e`
and size 892829 bytes. UART `reboot loader` succeeded. Final status confirmed
P1 active, valid, and carrying the unchanged checksum above, with
`last_result=0`; P2 and Stage retain the final App package. The App remains
unconfirmed and UART is released. This establishes these ten public cases
in this run, not self-update, BLE, full PAL, or long-duration acceptance.


## 2026-09-14 — review correction: single-level PAL mkdir

This follow-up to `0b982f45df8de6bcc33ab422a10f9012b2ca950a` supersedes the
recursive parent-creation behavior described above. The POSIX PAL implements
single-level mkdir; the earlier directory walk was a contract mismatch even
though public case 13 passed.

`ensure_directory` now checks only the direct parent with `directory_status`.
A missing parent returns NOT_FOUND without attempting creation; a parent that
is a regular file returns INVALID_STATE. The mounted SD root is exempt from
entry lookup, allowing initialization to create `/dl` and `/data` directly.
Only the leaf is passed to the verified trailing-slash SDK `fopen` path.
Existing-leaf, attribute, close, and post-create checks are retained, as is the
single-encoding `fopen` correction. Public test cases are unchanged. The final
diagnostic target additionally logs the missing-parent result and checks that
the parent remains absent.

`bazel test --config=macos_arm64 //tools/bazel:jieli_sd_directory_test`
passed with a fresh execution (17.7 seconds). The former recursive-creation
fixture now checks missing parents with zero creates, parent-is-file rejection,
leaf creation under an existing long-named parent, native creation failure,
and root-level `/dl` and `/data` creation/idempotence. The existing error and
buffer-boundary cases remain covered.

The native AC791N PAL package built successfully in 35.000 seconds in
`embed-zig-noble-amd64`, using the prescribed devenv, IDF-variable unsets,
`--output_user_root=/home/idy/.cache/bazel-ac791n/root`, `--config=ac791n`, and
`--symlink_prefix=bazel-amd64-`.

Package SHA-256:
`10c5c072d054371220244f5ace374677810dd4879aabce3567f984494702eb0d`.
UART send confirmed 881051 bytes with that digest. The port was re-listed and
released before use. Initial status confirmed UID `d879349abc9f`, P1 Loader
active, and the unchanged Loader checksum
`7130cfe2386c86a7dcf82ccd15854f14b64fb525be14a984cfdaa5dd19d64dfa`.
Installation used only UART `/dev/cu.usbserial-20131240` at 460800,
`send`, and `reboot upgrade --monitor` into P2.


The first single-level package captured Filesystem case 13 = 0, but only Core
suite entry afterwards; its monitor exited with code -7. Exclusive UART
status and return to Loader succeeded. Repeating upgrade with the retained
package, including after re-sending it, returned to Loader without running
App suites. A direct `reboot app` request was refused with device code -7.
These attempts provide no additional Core/Wi-Fi acceptance and are not UART
status failures. The Loader remained responsive throughout.

A final diagnostic build adds a missing-parent before/mkdir/after trace in
the E2E target, with no further board PAL changes. Native build passed in
35.902 seconds. Its package SHA-256 is:
`f2d528a110827e8161154d4c7df925103f1b6a76931454b3a5afc4aae631997d`.
UART send confirmed 881716 bytes and the matching digest, then installed it
into P2 through `reboot upgrade --monitor`.


Final-package board traces confirm the parent stays absent and all public
cases pass in the same run:

```text
H2_PAL_FS parent path=/dl stat=0 is_dir=1
H2_PAL_FS parent path=/data stat=0 is_dir=1
H2_PAL_FS missing_parent before=-8 mkdir=-8 after=-8
H2_PAL_FS mkdir path=storage/sd0/C/data/pal-host-e2e native=0 lookup=0
H2_PAL_E2E suite=64 case=13 result=0
H2_PAL_E2E suite=1 case=1 result=0
H2_PAL_E2E suite=1 case=2 result=0
H2_PAL_E2E suite=1 case=3 result=0
H2_PAL_E2E suite=1 case=4 result=0
H2_PAL_E2E suite=1 case=5 result=0
H2_PAL_E2E suite=1 case=6 result=0
H2_PAL_E2E suite=1 case=10 result=0
H2_PAL_E2E suite=1 case=11 result=0
H2_PAL_E2E suite=32 case=27 result=0
H2_PAL_E2E result=0 passed=10 failed=0
```

After stopping the monitor, exclusive UART status confirmed the final App
package in P2 and image SHA-256
`fec8c47945b29ac9f294d177d507d19705195c34ba8f76c63a9d60d5ff95cfb7`
(size 893693 bytes). This passing run does not explain the earlier incomplete
Core run or establish direct relaunch, self-update, BLE, or full PAL acceptance.
Captures are `tmp/jieli/2026-09-14-single-level-*.log`; the passing run uses
`single-level-final-monitor.log`.

UART return to Loader succeeded. Final status reported P1 active and valid,
its checksum unchanged, and `last_result=0`. P2 and Stage retain the final
package; the App remains unconfirmed. UART is released. No P1 write, USB
flash, physical recovery, or operation on another port occurred.
