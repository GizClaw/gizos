# Audio lifecycle repair — 2026-09-14

## SDK and reference behavior

The pinned SDK is `eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`. The
[source/IR ownership audit](./pal-review-followup-sdk.md) establishes that
`audio_pcm_play_data_write` can wait forever, its nonblocking full-buffer path
clears queued data, and `audio_pcm_play_stop` frees its handle without retaining
PAL writers. `server_close` drains server requests but is not evidence that
arbitrary PAL borrowers or decoder VFS waiters have retired. The cbuf reader
snapshots its length before locking; concurrent clear can invalidate it.

`libs/audio_mixer/src/h2_audio_mixer.c` retains operations through close, maps
closed operations to `H2_AUDIO_ERR_INVALID_STATE`, maps queue timeout to
`H2_AUDIO_ERR_WOULD_BLOCK`, and acknowledges a drain marker after the downstream
consumer takes prior queued frames. The new JieLi implementation follows those
semantics. Drain acknowledges consumption of prior PAL-buffered PCM; it does
not claim that the last DAC sample has physically left the speaker.

## Implementation

The board owns bounded PCM rings and connects directly to audio-server VFS.
The opaque SDK PCM helper cannot supply the required deadline, drain and
borrower-lifetime operations. Its now-unused runtime patch is removed from the
layout and firmware composition. No vendor checkout is changed.

- A PAL mutex/condition protects lifecycle, rings and predicates. Whole-frame
  writes use one deadline, including gate contention, and return WOULD_BLOCK
  when capacity is unavailable. No blocking PCM SDK write occurs under a lock.
- Drain captures the accepted-byte sequence and waits for the consumer to pass
  it. Future writes do not extend that wait.
- Close marks the track closing, wakes waiters, and waits for writers, drainers,
  volume requests and VFS callbacks before SDK STOP/close. In particular, a
  decoder's stack-owned condition waiter retires before SDK task deletion.
  STOP errors are returned; the void SDK close consumes the native server.
- VFS callbacks carry nonrecycled generation tokens and only access static
  slots after validating them under the gate. Late callbacks cannot reach a
  freed ring or publish microphone data into a restarted session.
- Microphone overflow drops oldest samples under the same gate as reads.
  Predicate waits replace semaphore tokens, so a successful immediate read
  cannot leave an obsolete wakeup for a later empty read.
- SDK OPEN/START/STOP/volume requests and synchronous callbacks execute outside
  the gate. Empty startup reads retain the SDK helper's nonblocking `-2`
  behavior. Mono-to-stereo conversion, 16 kHz format, DAC software AEC reference,
  default volume, track capacity and saturating microphone-monitor gain remain.
  Per-write UART diagnostics are removed from the deadline-bearing write path.

## Host evidence

`//tools/bazel:jieli_audio_lifecycle_test` compiles the real provider with a
pthread synchronization/audio-server fake and strict `-Wall -Wextra -Werror`.
The pre-fix SDK compatibility fake models its missing write deadline and its
cbuf lock/query boundary; it is not a second PAL implementation.

Before the replacement, assertions fail for full-buffer zero/finite writes,
drain-before-consumption, captured drain target, close with a retained writer,
a stale microphone callback after restart, a stale wake token, and a discarded
STOP error. The threaded microphone case reports a TSan data race between an
unlocked cbuf query and a locked write. A further deterministic interleaving
holds a woken decoder callback before condition-wait retirement: without the
VFS operation reference, SDK STOP is called too early; with it, close waits.

All cases pass after the repair with Clang, GCC in `embed-zig-noble-amd64`, and
TSan. The existing capacity fixture retains its SIZE_MAX and UINT32 overflow
cases. SDK startup/stop re-entry is exercised by the fake without deadlock.
Local logs are `/tmp/jieli-audio-before.log`, `/tmp/jieli-audio-tsan-before.log`,
`/tmp/jieli-audio-vfs-pin-before.log`, `/tmp/jieli-audio-after.log`,
`/tmp/jieli-audio-tsan.log`, and `/tmp/jieli-audio-gcc-after.log`.

## Hardware scope

The first audio probe exposed the separate task-affinity regression, repaired
in `c1f681df`; see [that comparison](./pal-task-affinity-hardware.json). The
replacement buffer path subsequently reached READY and produced 24 microphone
reports during a 30-second observation, before the final VFS retention change.
That intermediate result is not final-source acceptance.

The existing AC791N audio-system target has no stop/restart control. Startup
and microphone streaming can be observed on UART; a controlled stop/restart,
blocked-writer close, drain timing at the DAC, and injected SDK failures are not
covered by this hardware target. Host tests cover the corresponding PAL
lifecycle and failure scenarios. Final-source Loader self-install and the full
UART/BLE/PAL acceptance remain part of the overall review task.

## Final O1 package observations

[Hardware records](./pal-audio-lifecycle-hardware.json) distinguish the
intermediate stream from the final VFS-retention package. The final audio App,
UART-installed into P2, reaches READY and emits 25 microphone reports over a
30-second observation. Its package SHA-256 is
`8b09a2d3baab14dfecb90163e8b386ec21dcfaf239e6411b1834be14db3927b6`, image
`814a5edae7d32596da8bdeb2a99a1c230ce94339a1fff1310f5599d1fdc1a7d0`.
Loader, PAL and audio-system native packages build successfully (66.758
seconds).

The following untraced public PAL run completes **9/10**, not acceptance:
all eight Core cases (including 10 and 11) and offline Wi-Fi 27 pass;
Filesystem 13 returns `-7` at its first mkdir. UART reports the newly opened
creation handle as non-directory (`native=-7`), but reopening the requested
path verifies a directory (`lookup=0`). The test retains its failure and
performs its normal cleanup. No assertion, retry or timeout was weakened.
This SDK directory-handle discrepancy is tracked with O9 for source/IR audit;
it is not attributed to audio without evidence. Both Apps return through UART
to the same valid P1 Loader. Overall final-source PAL/UART/BLE acceptance
remains outstanding.
