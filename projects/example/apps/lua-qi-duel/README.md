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

Round resolution uses the user-approved **ElevenLabs Generation 2** rising,
strained charge cry (1.02 s), with surrounding silence removed and pitch/tempo
unchanged. Wave uses the final “哈” (0.74 s) from the user-selected ElevenLabs
generation W52tf8i2SyAxCSbgjo7P with the same James voice. Absorb uses the final
“呼” (0.57 s) from the user-approved ElevenLabs download, also with James.
Guard uses the opening grunt (0.67 s) from its user-approved ElevenLabs
download. All four skill cues use the same James voice. A Vegeta scream
excerpt (0.30 s) remains available as an asset. Round resolution plays only
the local player's skill cue; opponent actions and hit reactions are silent.

WAV previews contain only the short vocal. Embedded PCM adds 450/330/200 ms
before wave/absorb/guard, and 720 ms before the hit reaction, aligning with skill
windup and the 750 ms damage update. Charge starts immediately. Including these
delays, all cues finish by 1.19 s within the 1.8 s round animation.
Only the local skill track plays, at 45% gain, in both computer and network
matches. Selection and invalid actions are silent. Writes are nonblocking,
overdue cues are dropped, and preallocated tracks are reused across rounds and
restarts to avoid synchronous audio-device stalls. App shutdown closes the tracks. Devices
without audio continue silently; screenshot capture does not open real audio.

Listen to `assets/generated/sounds/*.wav`. [Source credits](assets/source/voices/CREDITS.md)
and `assets/source/voices/provenance.json` record upload labels, URLs, checksums
and excerpt offsets. The remaining third-party character recordings are not CC0 and
are not covered by the repository license; no commercial-use license is asserted.
Exact original scenes/dubs have not been independently verified.
WAVs are mono 16-bit / 16 kHz, peak -3 dBFS, with short fades; there is no pitch
shifting or time stretching. All four skill cues use generated speech; see their retained
originals and generation details in the source directory.

```sh
python3 projects/example/apps/lua-qi-duel/app/tools/generate_sounds.py
bazel test //projects/example/apps/lua-qi-duel/app:qi_duel_sounds_test
```

Build and run the real Lua Desktop target:

```sh
bazel run \
  //projects/example/targets/cc_binary/lua-qi-duel:example-lua-qi-duel
```

Build and run the H106 screen-sized native target:

```sh
bazel run \
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

Approved art is embedded as compressed RGBA/style/light resources. Lua drives
motion and effects; the native compositor submits RGB565 pixels to SDL. Runtime
does not require PNG decoding or a browser. Desktop fidelity currently uses a
14 MiB Lua VM budget; it is not an embedded-memory/performance validation.

Existing AMOLED package entry (not validated by the current desktop migration;
embedded resource assembly and memory/performance require separate work):

```sh
bazel build --config=esp32s3 \
  //projects/example/targets/h2loader_tar_zlib/lua-qi-duel/amoled:package
```

The runtime prints `H2_QI_DUEL_PERF` once per second. After five complete
measurement windows it prints `H2_QI_DUEL_SELF_TEST result=PASS` when the average
frame rate is between 27 and 32 FPS.
