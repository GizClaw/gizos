# Pref flash adapter bounds

The program callback already checked block bounds before address multiplication. Read and erase did not: with a 4096-byte block, block `0x100001` wrapped its 32-bit byte address to 4096 and passed the subsequent region check.

Read and erase now reject out-of-range blocks before multiplication, matching program. Read also rejects a transfer crossing its logical block boundary. The existing compiled adapter fixture now exercises read/program/erase, both with and without the diagnostic observer linked. It verifies invalid block, wrapped block and zero block size reject without physical read/erase calls; valid operations and driver failures are also checked. The fixture passed.

The Loader firmware built successfully using the AC791N Linux toolchain (34.715 seconds). Local log: `tmp/jieli/pref-bounds-native.log`.

This is defensive bounds hardening, not evidence that littlefs supplied such a block during the observed hardware failure. The old candidate Loader has not been overwritten and hardware recovery/self-update acceptance remain pending.

## Namespace open follow-up

Read-write namespace open previously accepted `LFS_ERR_EXIST` from mkdir without checking object type. It now stats that entry, accepts directories only, returns INVALID_STATE for a regular file, and propagates stat failure without publishing a namespace handle. The read-only path remains unchanged.

The existing compiled Pref fixtures passed five tests, including same-name file and stat-error injection, concurrent initialization/retry, commit/iteration failures and flash adapter bounds. Log: `tmp/jieli/pref-namespace-test.log`. This does not establish a cause for the pending hardware self-update failure.
