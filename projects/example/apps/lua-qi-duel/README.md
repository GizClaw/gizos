# Lua Qi Duel

`Lua Qi Duel` is a touch-first interface prototype for the game 《气决》,
with AMOLED 368×448 and H106 240×240 native desktop layouts.
The default app opens on a top-down particle field. One screen click starts an
eight-second Bluetooth search: a peer found in that window enters online play;
otherwise the same transition continues into a computer match. Particles run
slow-fast-slow, then the fake-3D camera tilts to the normal 45-degree view and the
remaining scene fades in. Combat starts at 5 HP / 0 qi with a visible three-second
choice timer, simultaneous resolution, three-hit full-qi combos and one-round
shield cooldown. See [GAMEPLAY.md](GAMEPLAY.md) for rules and protocol details.

Audio uses the reviewed retro electronic score: four looping scene tracks and
12 synthesized cues. Lua streams bounded 16 kHz mono S16LE chunks from note and
oscillator parameters; no recorded PCM clips are packaged. Scene transitions,
partial writes and cleanup are covered by `qi_duel_retro_audio_test`.
Audition exports and the approved score remain in `review/retro-audio-v1/`.

```sh
python3 projects/example/apps/lua-qi-duel/app/tools/generate_retro_audio.py
bazel test //projects/example/apps/lua-qi-duel/app:qi_duel_retro_audio_test
```

Build and run the real Lua Desktop target:

```sh
bazel run -c opt --define=h2_qi_duel_desktop_vectors=true --define=h2_qi_duel_vector_only=true \
  //projects/example/targets/cc_binary/lua-qi-duel:example-lua-qi-duel
```

Build and run the H106 screen-sized native target:

```sh
bazel run -c opt --define=h2_qi_duel_desktop_vectors=true --define=h2_qi_duel_vector_only=true \
  //projects/example/targets/cc_binary/lua-qi-duel:example-lua-qi-duel-h106
```

Both desktop targets use the same Lua resource, selecting layout from the actual
display dimensions. H106 uses uniform per-group transforms for the arena,
opponent, hands, two HUD panels and carousel; it does not stretch the portrait
screen into a square. Mouse hitboxes map back through the same transforms.
In `--rehearsal`, click a health/charge meter's left half to decrease, right half
to increase. Real battles disable these debug meter clicks.
Charge is now 0–5 with widened blue crystalline octagonal cells and no full-charge
flame. Use `--qi=5` to inspect it immediately. The charge orbit
and base are uniformly reduced to 78% of the earlier approved layout.
Click the desktop carousel's left/right region to rotate one position in that
direction; click its center to cast. See `SDL-MIGRATION.md` for fidelity,
asset provenance, screenshot probes and test evidence.

In rehearsal, both combatants demonstrate charge, wave, absorb or guard. Desktop drags cancel
clicks; firmware touch still uses horizontal selection and upward flick casting.
See `ACTION-PREVIEW.md` for controls, timing and
deterministic captures. Device performance parity remains unverified.
H106 keys: Volume+ / Volume- switch the wheel; Record casts. Desktop equivalents
are Up / Down / Tab. The icon remains stationary while an expanding echo fades.
Full-qi waves show a large shaking `COMBO` impact word; a broken Guard follows
with `ARMOR BREAK`, using the same authored pixel-art language as the countdown.
When both players fire, combat cuts to a pure-black diagonal beam-clash close-up:
cyan enters broad from the near lower-left, violet enters thin from the far
upper-right, and a combo progressively pushes the contact point toward the
weaker side. The collision now decays through authored spark frames instead of
cutting away; a non-final clash then fades the arena back in. On game over,
`YOU` slides from the left while `WIN` or `LOSE` slides from the right, then both
light over parallel, independently moving blue/violet (win) or orange/red (lose)
bars. Clicking after the entry sends the words off opposite edges and lets the
existing bars clear before pairing restarts. Use
`--clash=equal|player-combo|enemy-combo|both-combo` or `--result=win|lose` with
`--time-ms=N` for deterministic SDL frame review.

On macOS, the commands above select the accepted vector artwork and analytic Lua
scene lights. The native compositor submits RGB565 pixels to SDL. The original
RGBA/style/light resources remain available for deterministic reference captures;
they are excluded from the vector-only build. See [VECTOR-DESKTOP.md](VECTOR-DESKTOP.md)
for build modes, current poses, comparison metrics and regeneration instructions.
The 14 MiB desktop Lua VM budget does not establish embedded memory feasibility.

AMOLED pure-vector test firmware (hardware performance acceptance pending;
see [AMOLED-MIGRATION.md](AMOLED-MIGRATION.md) for the download partition limit):

```sh
bazel build -c opt --config=esp32s3 \
  --define=h2_qi_duel_software_vectors=true \
  --define=h2_qi_duel_screen=amoled \
  --//tools/bazel:firmware_version=0.1.0-dev \
  //projects/example/targets/h2loader_tar_zlib/lua-qi-duel/amoled:package
```

The screen flag removes the other screen's clash keyframes at build time.
Omitting it retains both screens for desktop review; `h106` selects H106-only
frames. Retained component keyframes use lossless H2VG v2 coordinate deltas, and
every build reconstructs and compares their original command bytes before
embedding. The packer omits unreachable action poses, reuses one affine charge
orientation, and replaces the duplicate clash-fade bank with runtime opacity.

The runtime prints `H2_QI_DUEL_PERF` once per second. After five complete
measurement windows it prints `H2_QI_DUEL_SELF_TEST result=PASS` when the average
frame rate is between 27 and 32 FPS.

Countdown accelerates by round: rounds 1–7 use 1000 ms per digit (3 s total),
rounds 8–14 use 800 ms (2.4 s), and round 15 onward uses 600 ms (1.8 s).
Local and network deadlines and displayed digits share this timing. A new match
starts at round 1; early confirmations do not shorten the countdown.

The private Zero ESP32-S3 independent firmware build and its validation limits are documented in [ZERO-STANDALONE.md](ZERO-STANDALONE.md).
