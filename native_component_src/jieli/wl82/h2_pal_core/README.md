# WL82 core PAL SDK boundary

## Non-blocking synchronization

PAL timeout `0` means try without waiting. The SDK's `os_mutex_pend` and `os_sem_pend` instead interpret timeout `0` as an infinite wait. Therefore `h2_jieli_wl82_sdk_port.c` uses the public `os_mutex_accept` and `os_sem_accept` operations for this case; substituting `pend(..., 0)` would introduce a hang.

The pinned AC791N SDK revision is recorded in `tools/bazel/native_versions/jieli_ac791n_sdk_commit.txt`. At revision `eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`:

- `include_lib/system/os/os_api.h:355` declares `int os_sem_accept(OS_SEM *sem)`.
- The same header at line 440 declares `int os_mutex_accept(OS_MUTEX *mutex)`.
- Both are documented as non-blocking, returning `0` on success and `OS_TIMEOUT` if acquisition fails. Neither may be called from an ISR or a critical section.
- `cpu/wl82/liba/system.a`, member `os_api.c.o`, exports both as defined text symbols, not unresolved references.

### Reproduce the export check

From the SDK checkout matching the pinned revision, use GNU `nm` (the SDK archive is not a macOS Mach-O library):

```sh
git rev-parse HEAD
rg -n 'int os_(sem|mutex)_accept' include_lib/system/os/os_api.h
nm -A cpu/wl82/liba/system.a | rg ' T os_(mutex|sem)_accept$'
```

The defined-symbol lines observed for this revision are:

```text
cpu/wl82/liba/system.a:os_api.c.o:00000000 T os_mutex_accept
cpu/wl82/liba/system.a:os_api.c.o:00000000 T os_sem_accept
```

The shared DevKit layout passes `sdk_abi.ld` to the actual native linker. `EXTERN` retains both operations through LTO/GC, and `ASSERT(DEFINED(...))` rejects missing definitions before any firmware can be packaged. `tools/bazel/tests/test_jieli_sdk_abi.py` tests the linker script with both definitions present and with each definition individually missing; run it on Linux with `python3 -m unittest tools.bazel.tests.test_jieli_sdk_abi`.

When changing the SDK revision, repeat the export check and link the actual firmware using the configured AC791N SDK/toolchain environment:

```sh
bazel build --config=ac791n \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/jieli_ac791n_devkit:package \
  //projects/example/targets/h2loader_tar_zlib/display/jieli_ac791n_devkit:mp4_player_small_package
```

Export and link checks establish symbol availability only; they do not substitute for on-device synchronization and boot-lifecycle acceptance.
