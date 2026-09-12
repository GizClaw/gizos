# Loader copy interruption: physical power cycle

Diagnostic package `loader-copy-powercut-v2.tar.zlib` (917410 bytes), SHA-256
`5f3ec889992d71e8c0a7e73701cb03ec94002d85cf8d9c340f325f2bc4fbcaf4`.
Image SHA-256 `7f922ab99fede5e41915d0fe96b93a854b8741c87645c67017f1a8480baa2fe8`.

Installed through UART1 H2Loader at 460800, not USB DL. The diagnostic
candidate uses the production layout, launcher and task policy. Its erase
observer pauses after physical readback establishes P1's 32-byte boot header
is erased and P2's header is present. The normal confirmation/publication
gate runs before this observer. The watchdog is disabled only at the pause.

Observed sequence in `tmp/jieli/powercut-v2-install-monitor.log`:

1. P2 candidate confirmation, `LOADER_HEADER published=1`, copy event 2.
2. Repeated `POWERCUT_READY p1_header=erased p2_header=present`.
3. User switched power off/on; SDK reports `system reset reason: POWER ON`.
4. Copy completes with digest `7f922ab9...`, then
   `POWER_REBOOT running=2 next=1 committed=1`.
5. P1 native BootInfo: base `0x4020`, version 16, followed by event 4 / READY.
6. Monitor stopped; independent UART status exits successfully: running/next
   P1, both partition image checksums equal the full SHA above, Stage empty,
   `last_result=0`, UID `3ce9e275d7aa`.

Status source: `tmp/jieli/powercut-v2-after.status`. No host flashing or reboot
command was issued between the pause and the physical restart. The fixture
skips its pause on restart when P1's header is already erased.

Scope: proves recovery for this interrupted P2-to-P1 copy boundary on this
board. Does not prove arbitrary partial NOR programming, partial native boot
headers, or all Preference torn-write patterns. SDK VM emitted a CRC warning
after power-on; prolonged diagnostic pause also emitted UART circular-buffer
overflow messages. These are retained observations, not silently treated as
passing log-health tests. The installed image is still the diagnostic Loader,
not the production release artifact.
