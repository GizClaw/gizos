# BK Preferences large-value backing

The fixed SDK (`aa5df964b0f64924ee6d0d2ffd6c3ca6ed59f9ca`) cannot store a
16 KiB KV or a 4095-byte string in its original 4 KiB FlashDB sector. Increasing
only the database length does not remove that single-record limit. Three 32 KiB
sectors also fail the real-engine overwrite/GC test with two resident large keys.

The physical partition already owns `0x780000..0x79ffff` (128 KiB). The FAL table
now splits it without moving any physical boundary:

| FAL path | Address | Size | FlashDB sector |
| --- | --- | --- | --- |
| `h2_pref` | `0x780000` | 24 KiB | 4 KiB, original encoding |
| `h2_pref_large` | `0x786000` | 104 KiB | 4 KiB, new separate database |

`coredump`, EasyFlash, WiFi, boot control and installed P1 addresses are unchanged.
The SDK FlashDB component/sample implementation is disabled; the PAL-owned
FlashDB/FAL source list has no TSDB or other tail consumer in this board profile.

Ordinary values up to 3968 bytes stay in the original DB, with existing type
sidecars. A larger value uses immutable 3000-byte chunks plus owner/generation
headers in the new DB, followed by one 96-byte manifest. Each chunk and manifest
is a real FlashDB record, covered by its CRC, status transitions and GC. Reads
validate complete chunk ownership, index, generation, length and aggregate hash.
The local maximum is 32 KiB; total capacity still depends on other live values.
After a key enters the new DB, subsequent smaller replacements stay there.
Iteration returns the logical value size and one key; remove/clear delete the
legacy duplicate before removing the authoritative large manifest and chunks.
Partial-creation chunks are collected by namespace clear using the staged
original type sidecar. Internal names cannot collide with a valid namespace:
their first dot is beyond the maximum 15-byte namespace.

Initialization publishes the reserved, non-namespace `$h2_large_backing_v1`
ownership record only after verifying the entire new tail is erased. Unknown
nonempty tail contents are rejected. An owned invalid sector header is repairable
only when the entire sector payload is erased; surviving payload is never erased
merely to make initialization succeed. The extra small ownership record uses the
old FlashDB format and is ignored by old P1 namespace operations.

The fixed SDK's `move_kv` ignores intermediate copy/status errors, `do_gc`
erases despite a failed move, `del_kv` retains a block-scoped pointer beyond its
lifetime, and interrupted-record scanning can stop before later recovery work.
The SHA-locked build overlay fixes these paths without editing the SDK checkout
or changing record encoding. It preserves live sectors on failed relocation,
continues past a space shortage to collect later garbage, and checks each copy
before retiring its source. The production FAL port also latches hardware I/O
failure: remaining writes/erases of that operation are refused, its result is I/O,
and the next PAL operation first reinitializes the real databases for recovery.
The test compiles the exact same generator output as the native CMake build.

Writes remain synchronous. A failure before manifest publication retains the old
large generation. An error after full publication can leave the complete new
value committed, matching existing FlashDB behavior; the selected type must match
its bytes. This is not a new cross-provider transaction or power-loss guarantee.
The NOR model exercises failed programming, erased-header recovery and erase
errors before/after a complete 4 KiB erase, not arbitrary partial-sector damage.

Run the real engine test explicitly with an unchanged pinned SDK and native C
compiler:

```sh
BK7258_PATH=/path/to/bk-avdk-smp bazel test --config=macos_arm64 \
  --cache_test_results=no \
  //native_component_src/bk7258/ap/h2_pal_core:pref_flashdb_real_nor_test
```

It reproduces the original single-KV limit, then verifies two resident 16 KiB
values, 4095-byte strings, 80 large overwrites with real GC, 1000 small overwrites,
independent-process persistence/cleanup, old P1 raw reads, 72 program injection
positions (68 actually fault), same-process/read-error recovery, erase errors,
and type changes. Only RTOS/NOR hardware and the separate EasyFlash migration
boundary are replaced; no in-memory FlashDB shim is used. The old
`pref_flashdb_test` remains a separate SDK-boundary/type regression test.

For ASan or private device-backup compatibility:

```sh
H2_PREF_NOR_SANITIZE=1 BK7258_PATH=/path/to/bk-avdk-smp \
  python3 native_component_src/bk7258/ap/h2_pal_core/tests/run_pref_flashdb_nor.py

BK7258_PATH=/path/to/bk-avdk-smp \
  python3 native_component_src/bk7258/ap/h2_pal_core/tests/run_pref_flashdb_nor.py \
  --sample /private/flashdb-128k.bin --sample-sha256 EXPECTED_SHA256
```

The optional sample is copied into a private temporary NOR image. The original is
read-only. Compatibility compares every original raw KV and type sidecar before
initialization and after large-value conversion, removal, restoration and a fresh
process. Only counts and canonical SHA256 values are printed; device values and
private images must never be checked in. Host NOR success is not board
qualification: the unchanged 36-case Pref contract still needs the new actual BK
artifact to pass five fresh boots, `1/2/3/4/4`.


## Bounded read and route caches

The AP artifact uses the SDK Flash client; CP owns the physical Flash driver.
FlashDB verifies each KV CRC in 32-byte reads, while each AP read performs both
READ and READ_DONE mailbox handshakes. The SDK IPC payload is 512 bytes. The FAL
port now keeps one 512-byte read-ahead window only during a serialized FlashDB
operation; begin/end, write/erase attempts, read errors and recovery invalidate
it under the Flash mutex. Filling stops at the current FAL partition boundary,
never skips CRC verification, and a failed fill still returns I/O. Initialization
is explicitly scoped before the normal DB callbacks are registered.

A 16-entry volatile cache remembers only a complete logical key and its selected
DB. It caches both large presence and absence, never a record address, payload or
type; GC can relocate records without changing that selection. Any large mutation
attempt, I/O failure or recovery invalidates it before later reads can select an
old small duplicate. Successful publication can repopulate the known route.
Namespace snapshots check their prefix before querying large presence, so unrelated
old keys do not cause full large-DB scans. No on-flash record changes are involved.

The pinned real NOR comparison uses a resident 16 KiB value and 200 consecutive
4-byte counter replacements. Pre-cache production source `329cbe5f` performs
441098 hardware reads / 13850774 bytes; the cached source performs 7500 reads /
3833587 bytes (58.81x fewer hardware calls). This is a call-count result, not a
board time or qualification claim. Run the comparison with `--compare-baseline`.
The suite additionally checks a cached miss followed by creation, big-to-small
overwrite, deletion/clear and a driver error after a complete manifest WRITE flag
was programmed, ensuring recovery cannot choose the retained old small raw value.
All previous real NOR faults, private-sample compatibility and ASan remain required.
Historical r2 board source/package identity must not be rebound to this performance
change.
