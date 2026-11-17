# Fruit Slice sound bank

All effects, the melody and the nonverbal vocal cheer are original mathematical synthesis authored for Fruit Slice. The generator and its generated PCM/C assets are distributed under the repository's [Apache-2.0 license](../../../../LICENSE). They contain no recordings, sound fonts, sampled human voices, external melodies or system sounds. No service, model weights, network access or third-party Python package is needed to rebuild them.

```sh
python3 projects/fruit_slice/app/resources/generate_audio.py
python3 projects/fruit_slice/app/resources/generate_audio.py --preview-dir /tmp/fruit-slice-audio
```

The checked-in `src/h2_fruit_slice_audio_bank.c` is an explicit input to the App's Bazel library and is linked into every artifact through the normal firmware/App graph. There are no external runtime file paths or unpacked sidecar assets. [`audio_manifest.json`](audio_manifest.json) records each signed PCM8 clip's sample count and SHA-256. WAV previews are optional unsigned PCM8 exports and should remain outside the source tree. The bank is 98,704 bytes of read-only PCM, including 64,000 bytes for music. Frames expand and linearly interpolate the 8 kHz mono bank into the Audio PAL's native S16 PCM format (8–48 kHz, one or two channels); there is no runtime decoder or floating-point synthesis.

| Event | Design |
| --- | --- |
| SWING | 75 ms shaped noise sweep |
| SLICE | Wet noise transient and falling pitched impact |
| COMBO | Ascending three-note chime |
| BOMB | 420 ms low-passed noise and falling bass impact |
| MISS | Short descending warning |
| FAIL | Descending minor cadence |
| RESULT | Delayed ascending score cadence |
| COIN | Two high metallic notes |
| TASK | Four-note achievement fanfare |
| PURCHASE | Three-note purchase confirmation |
| EQUIP | Short click and bright confirmation tone |
| CARD | Three-note reveal shimmer |
| RARE | Longer five-note rare-card fanfare |
| CHEER | Original 420 ms formant-synthesized, nonverbal “woo” |
| Music | Original 8-second, four-bar, 120 BPM C–Am–F–G arcade loop |

The cheer is robotic vowel synthesis, not recorded speech or a TTS word prompt. This change does not claim Mandarin/English phrase narration. The local machine provided macOS `say` and ffmpeg, but no installed open TTS engine; system voice redistribution was not assumed. The bank's generator defines every oscillator, deterministic noise seed, note, envelope and vowel formant directly.

`h2_fruit_slice_audio` owns one native Audio PAL track, four fixed effect voices, and one native PCM frame allocation (at most 8,192 bytes; desktop 640 bytes, iOS/Android 1,024 bytes, current Web 1,920 bytes; excluding the existing provider's device/ring buffers). It uses 32-bit accumulation with signed-16 saturation. Music peaks well below SFX and ducks further while any effect plays. Duplicate slice/swing bursts have 35/100 ms cooldowns; other effects have a 180 ms cooldown. A bomb clears old effects. A higher-priority effect replaces a lower-priority voice; lower-priority events cannot evict higher-priority ones. PCM writes use timeout zero and only three native frames may be submitted per game tick; stalled/full playback discards stale frames rather than accumulating work. No audio worker, mic, per-effect allocation or filesystem decode runs during play.

Title-screen SFX and BGM buttons independently toggle mute and persist a versioned `fruit_slice/audio_v1` u32, separate from `best`, `progress_v1` and card data. Changes immediately close the current track to flush queued PCM. Missing Preferences retain settings in memory. Playback starts only after the first pointer-down gesture so browser autoplay starts through the existing Web Audio provider. The Web provider itself adds an 80 ms starting cushion; current native frame size adds up to 60 ms. This bank does not remove that provider-level latency. Mobile activity/scene pause, browser visibility and desktop focus close the track and stop the speaker; resuming creates a fresh track, and App exit joins provider shutdown before freeing its buffers. Platforms without usable playback log an optional-audio failure and continue the game.

## Integration

The deterministic game model owns a 16-element queue and calls `h2_fruit_slice_game_sound(game, H2_FRUIT_SLICE_SOUND_...)`; the App drains it into `h2_fruit_slice_audio_emit()` after input/simulation and pumps PCM before rendering. The queue coalesces swing/slice/coin bursts within one drain. On saturation a critical request replaces a low-priority queued request; if no such slot exists it returns false. The game queue does not borrow an Audio PAL or modify a worker-owned object.

Base gameplay already emits SWING, SLICE, COMBO/CHEER, BOMB, MISS, FAIL and a one-shot RESULT after the restart delay. For the parallel economy/card integration, enqueue TASK and COIN after a successful task reward, PURCHASE immediately after debiting coins and recording ownership, EQUIP after changing the equipped weapon, CARD only for a newly unlocked card, and RARE for a rare-card reveal. Do not emit these events for insufficient funds, unchanged equipment, duplicate cards or a canceled touch. `h2_fruit_slice_game_sound()` is declared in the private game header and the enum in `h2_fruit_slice_sound.h`.

## Validation boundary

The owning Bazel `audio_test` checks that every bank event produces nonzero PCM, burst limits, voice priority, bounded recovery after a stall, full-queue behavior, independent mute persistence, corrupt preferences, unsupported audio, partial initialization and shutdown. `app_test` drives the public run loop with real Runtime wrapping fake PCM playback, touch input, pause/resume and exit, and checks that every allocation/track/speaker is released. These checks do not establish subjective listening quality or on-device latency; physical speaker/headphone listening and device CPU/latency measurements remain separate acceptance work.
