# Lua Qi Duel

`Lua Qi Duel` is a touch-first interface prototype for the game 《气决》,
with AMOLED 368×448 and H106 240×240 native desktop layouts.
The default desktop app now opens **面对心魔** (computer opponent) and **挑战虚空**
(click to pair) modes. Both start at 5 HP / 0 qi, with a visible two-second choice
timer, simultaneous resolution, three-hit full-qi combos and one-round shield
cooldown. Missed decisions are empty moves. See [GAMEPLAY.md](GAMEPLAY.md) for
rules, protocol, tests and the real-BLE provider limitations.

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
