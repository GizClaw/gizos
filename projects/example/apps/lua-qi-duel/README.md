# Lua Qi Duel

`Lua Qi Duel` is a touch-first AMOLED interface prototype for the game 《气决》.
The first milestone intentionally omits Bluetooth pairing and combat resolution.
It validates the visual hierarchy and device performance with a moving particle
arena, an animated opponent, first-person idle hands, and a swipeable four-skill
carousel (Charge, Wave, Absorb, and Guard).

Build and run the real Lua Desktop target:

```sh
bazel run \
  //projects/example/targets/cc_binary/lua-qi-duel:example-lua-qi-duel
```

The Desktop and AMOLED targets execute the same embedded Lua resource. Approved
PNG art sources are converted into code-owned ARGB4444 resources by
`app/tools/generate_assets.py`; Lua composites them through `display.draw_asset`
into the RGB565 framebuffer. Runtime code does not read PNG files or a browser
preview.

Build the AMOLED package:

```sh
bazel build --config=esp32s3 \
  //projects/example/targets/h2loader_tar_zlib/lua-qi-duel/amoled:package
```

The runtime prints `H2_QI_DUEL_PERF` once per second. After five complete
measurement windows it prints `H2_QI_DUEL_SELF_TEST result=PASS` when the average
frame rate is between 27 and 32 FPS.
