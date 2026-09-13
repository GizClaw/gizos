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
