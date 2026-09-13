# PAL review follow-up: pinned SDK inspection

SDK checkout: `/Users/idy/h2vivi/firmwares-devenv/third_party/jieli_ac791n_sdk`,
verified revision `eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`.
No SDK checkout or vendor source was modified.

Archive SHA-256:

| Archive | SHA-256 |
| --- | --- |
| `cpu/wl82/liba/system.a` | `924b12f561aa28ee0b653903946c1c420803151d4286d7f825adb22e1024cbba` |
| `cpu/wl82/liba/update.a` | `30d61af6c11905a6a8101027f731538313be1d6810931712672be1eb3d7eb05e` |

## Reproduction

In OrbStack `embed-zig-noble-amd64`, extract archive members into a temporary
directory using `ar x`. The pinned toolchain's
`pi32v2/bin/clang -x ir -S -emit-llvm -Wno-override-module <member> -o <member>.ll`
prints their LLVM bitcode. This is inspection only: no emitted IR was compiled
or executed as host firmware. The emitter changes the displayed target triple;
these observations concern the original function calls/control flow, not a
host ABI. Local extracts remain in `/tmp/jieli-pal-review-sdk` in the VM.

## O1: PCM and microphone ownership

`apps/common/audio_music/pcm_play_api.c:291` implements stop by clearing
`run_flag`, posting writer/reader semaphores, requesting decoder stop, calling
`server_close`, deleting semaphores, and freeing cache and handle. It does not
join PAL callers blocked inside `audio_pcm_play_data_write`. The nonblocking
write branch at line 345 clears its cbuf when a write does not fit. Merely
changing `block` therefore does not implement PAL backpressure.

`system.a:server_core.c.o`, `server_close` (emitted IR lines 1120 onward), marks
the server unavailable under its mutex, waits for the pending-request list,
invokes its service close function and frees the server. This establishes
server request draining; it does not by itself prove the audio service's VFS
callback quiescence or safe PAL writer lifetime.

`system.a:circular_buf.c.o`, `cbuf_read` (IR line 10), reads `data_len` before
acquiring the IRQ/testset lock, then consumes that snapshot under the lock.
`cbuf_clear` (IR line 602) resets pointers/data length under the same lock.
The existence of the native lock alone therefore does not establish that a
concurrent clear cannot invalidate the reader's earlier size snapshot.

O1 remains open. Recommended implementation is a PAL-owned queue/mixer and
explicit producer/consumer shutdown, with real pthread/TSan blocked-writer,
reader, close and restart tests, followed by audio hardware acceptance.

## O8: native failure boundaries and allocator decision

`system.a:os_api.c.o`, `os_mutex_del` (IR line 2556) and `os_sem_del`
(IR line 2406), both call `vQueueDelete` and unconditionally return 0. For this
pinned implementation, the wrapper does not lose a returned native delete
error. Deleting a live object is still invalid; this is not evidence that
active waiters may be freed safely.

`os_mutex_post` calls `xQueueGiveMutexRecursive` and maps a non-success return
to `-14`; IRQ/disabled-IRQ invocation has separate assert/reset handling.
`os_sem_post` calls `xQueueGenericSend` (or the ISR equivalent) and also maps
failure to `-14`. Their complete valid-object failure reachability has not yet
been established. No claim that unlock or queue-wake errors are impossible.

Allocator options remain a product decision: (1) support the caller allocator
consistently for object allocation/free, following the reference providers;
(2) explicitly restrict this platform to the SDK heap and reject unsupported
allocator configurations. Recommendation: option 1. No allocator policy was
changed; user confirmation is pending.

## O11: erase callback return is discarded by the updater

`update.a:dev_upgrade_api.c.o`, `dev_upgrade_erase` (IR line 162), calls
`norflash_ioctl` and returns 1 for a recognized command without testing that
call's result, matching the repository adapter's existing convention.

`update.a:flash_fs_api.c.o`, `flash_erase_by_blcok_n_sector` (IR line 216),
calls its callback for pages, sectors and blocks. For example `%call` at
line 267, `%call40` at line 325 and `%call68` at line 400 are not consumed by
any success/failure branch. The helper returns void and advances the erase
address. Changing only the callback's return convention cannot propagate a
failure through this helper.

`dual_bank_passive_update.c.o` also discards the direct header-write result
and subsequently compares an origin-read header against the expected header
(IR lines 2954-2960). This is header readback evidence, not proof of complete
failed-erase detection across every updater path.

O11 remains open. Recommended next implementation is an operation-wide erase
failure latch checked before further writes and before publishing successful
completion, with fault injection through the updater boundary. A direct
`return 0` change alone would not fix the demonstrated ignored return path.
