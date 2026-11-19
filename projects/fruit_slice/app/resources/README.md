# Fruit Slice sound bank

All effects, the melody and the nonverbal vocal cheer are original mathematical synthesis authored for Fruit Slice. The generator and its generated PCM/C assets are distributed under the repository's [Apache-2.0 license](../../../../LICENSE). They contain no recordings, sound fonts, sampled human voices, external melodies or system sounds. No service, model weights, network access or third-party Python package is needed to rebuild them.

```sh
python3 projects/fruit_slice/app/resources/generate_audio.py
python3 projects/fruit_slice/app/resources/generate_audio.py --preview-dir /tmp/fruit-slice-audio
```

The checked-in `src/h2_fruit_slice_audio_bank.c` is an explicit input to the App's Bazel library and is linked into every artifact through the normal firmware/App graph. There are no external runtime file paths or unpacked sidecar assets. [`audio_manifest.json`](audio_manifest.json) records each signed PCM8 clip's sample count and SHA-256. WAV previews are optional unsigned PCM8 exports and should remain outside the source tree. The bank is 100,272 bytes of read-only PCM, including 64,000 bytes for music. The five additional hits total 1,888 bytes. Frames expand and linearly interpolate the 8 kHz mono bank into the Audio PAL's native S16 PCM format (8–48 kHz, one or two channels); there is no runtime decoder or floating-point synthesis.

| Event | Design |
| --- | --- |
| SWING | 75 ms shaped noise sweep |
| Classic Knife | 60 ms unpitched peel snap and pulp texture from the original slice bank |
| Jade Katana | 50 ms thin, bright cut with a wet onset and a quick sheen |
| Flame Axe | 48 ms weighty low-mid knock and pulp crunch |
| Bamboo Saber | 38 ms dry double knock with two wood resonances |
| Frost Cleaver | 60 ms rind hit followed by staggered ice crackles |
| Storm Blade | 40 ms immediate crack and close secondary sparks |
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

The Classic Knife uses the original SLICE clip. Its generator uses 480 samples at 8 kHz, seeded noise (`0x534C4943`), two cascaded 1.4 kHz low-pass stages and a 220 Hz low-frequency subtraction. A 3.6 ms snap decay sits over a 12 ms pulp decay, with small noise grains at 7/12/20 ms and smoothed onset/end. Its source multiplier is 1.9 before the existing shared PCM8 quantizer. The five additional hits use separate noise seeds, band-pass filters and transient envelopes. Their lengths and brief tails keep successive cuts perceptible on the 16 kHz mono output. All six use the same playback gain.

The cheer is robotic vowel synthesis, not recorded speech or a TTS word prompt. This change does not claim Mandarin/English phrase narration. The local machine provided macOS `say` and ffmpeg, but no installed open TTS engine; system voice redistribution was not assumed. The bank's generator defines every oscillator, deterministic noise seed, note, envelope and vowel formant directly.

`h2_fruit_slice_audio` owns one native Audio PAL track, four fixed effect voices, and one native PCM frame allocation (at most 8,192 bytes; desktop 640 bytes, iOS/Android 1,024 bytes, current Web 1,920 bytes; excluding the existing provider's device/ring buffers). It uses 32-bit accumulation with signed-16 saturation. Music peaks well below SFX and ducks further while any effect plays. Duplicate slice/swing bursts have 35/100 ms cooldowns. Other effects have a 180 ms cooldown. A bomb clears old effects. A higher-priority effect can replace a lower-priority voice. PCM writes use timeout zero, with at most three native frames per pump. Stalled or full playback discards stale frames. A cut needs no per-effect allocation or filesystem decode.

Title-screen SFX and BGM buttons independently toggle mute and persist a versioned `fruit_slice/audio_v1` u32, separate from `best`, `progress_v1` and card data. A mute change stops playback and closes the current track to flush queued PCM. If shutdown fails, the App retries before applying the new mute setting. Missing Preferences retain settings in memory. Playback starts only after the first pointer-down gesture so browser autoplay starts through the existing Web Audio provider. The Web provider itself adds an 80 ms starting cushion; current native frame size adds up to 60 ms. This bank does not remove that provider-level latency. Mobile activity/scene pause, browser visibility and desktop focus close the track and stop the speaker; resuming creates a fresh track, and App exit joins provider shutdown before freeing its buffers. On iOS, close preserves the existing tail drain of up to 3 seconds, then uses AudioQueueReset to cancel remaining PCM and consumes the valid track on every result, returning the first drain/reset error. The App attempts speaker shutdown after a close error, retains any remaining cleanup resources for retry, and returns the first cleanup error after reclamation. Desktop real-device capability reporting treats input and output independently: playback needs an output device, while recording still needs an input device. Platforms without usable playback log an optional-audio failure and continue the game.

## Integration

The deterministic game model owns a 16-element queue and calls `h2_fruit_slice_game_sound(game, H2_FRUIT_SLICE_SOUND_...)`. The App drains it into `h2_fruit_slice_audio_emit()` after input and simulation. The queue coalesces duplicate swing, hit and coin cues within one drain. On saturation a critical request replaces a low-priority queued request, or returns false if all queued requests are critical. AC791N enables the independent PCM producer, which receives the selected cue through a bounded PAL queue and owns the same Audio PAL track. On cooperative targets the App services PCM itself.

Base gameplay emits SWING, equipped-weapon fruit hits, COMBO/CHEER, BOMB, MISS, FAIL and a one-shot RESULT after the restart delay. The equipped weapon's hit sound is selected on contact with fruit, including menu fruit. All hit variants share the existing 35 ms cooldown and voice priority. A long stroke can also emit the shared SWING cue. Successful task rewards emit TASK and COIN. A first weapon purchase emits PURCHASE after debiting coins and recording ownership, then EQUIP for the actual equipment change. Switching owned weapons emits only EQUIP. Duplicate claims, insufficient funds, unchanged equipment and canceled touches emit no economy confirmation. `h2_fruit_slice_game_sound()` is declared in the private game header and the enum in `h2_fruit_slice_sound.h`.

## Validation boundary

The owning Bazel `audio_test` checks that every bank event produces nonzero PCM, burst limits, voice priority, bounded recovery after a stall, full-queue behavior, independent mute persistence, corrupt preferences, unsupported audio, partial initialization and shutdown. `app_test` drives the public run loop with real Runtime wrapping fake PCM playback, touch input, pause/resume and exit, including consuming close errors and retriable speaker-stop failures, and checks that every allocation/track/speaker is released. The iOS PAL XCTest compiles the actual iOS provider with injected wait/AudioQueueReset failures and checks explicit drain, consuming close, Runtime wrapper/backend release and a new track after close. PortAudio's provider test injects input-only, output-only, duplex and absent default devices through the same enumeration helper used by get_info. These checks do not establish subjective listening quality or on-device latency; physical speaker/headphone listening and device CPU/latency measurements remain separate acceptance work.
