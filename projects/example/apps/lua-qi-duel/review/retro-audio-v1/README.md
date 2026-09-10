# Retro electronic candidate 1 — awaiting user review

Generated from original note, rhythm, oscillator and sweep parameters by
`app/tools/generate_retro_audio.py`. No recording samples or AI music files are
inputs. The existing selected audio and gameplay integration are unchanged.

Audition `desktop-preview/retro-audio-review.html` through the local review
server. It contains four music tracks, twelve effects, a 26-second context mix
and a sequential effects preview. Music may be played underneath an effect.
The page stores review notes locally and can export them; it never turns a
rating into automatic production approval.

`score.lua` holds 17,354 bytes of event parameters without any PCM samples.
`score.json` adds readable authoring metadata. WAVs are 16 kHz mono 16-bit
review renders only; do not embed them as the final synthesized soundtrack.
The audition engine uses pulse waves with polyBLEP, triangle and sine-based
voices, deterministic noise, envelopes and a DC blocker. A final streaming
Lua/native synthesizer still needs to be implemented and measured on device.
Maximum authored music polyphony is six voices before overlaid game effects.

All 18 WAV outputs were checked for their format, non-silence, sample hash,
absence of PCM clipping, and zero endpoints for the two looping tracks.
The browser loaded the context mix and all four music streams without media
errors. These checks establish technical integrity, not subjective approval
or device CPU/RAM suitability. `manifest.json` identifies the candidate audio.
