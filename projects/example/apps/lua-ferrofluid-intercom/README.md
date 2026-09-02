# Lua Particle Intercom

`Lua Particle Intercom` is the portable audio-reactive UI demo for the first
stage of a Bluetooth intercom. The same Lua scene runs on Desktop and the
368x448 AMOLED board. It captures microphone frames through Runtime Audio and
maps RMS, peak, and high-frequency energy to a central particle bloom on a pure
black background.

The particle lifecycle follows the `PARTICLES` mode from SonicVis: quiet input
continuously emits a small ambient stream, audible peaks launch faster and
larger bursts, velocity decays with friction, and each particle shrinks and
fades with its remaining life. The emitter stays centered and deliberately
ignores touch and mouse input; animation intensity comes only from audio and
the quiet idle stream.

Trails are retained framebuffer history, not line primitives. Every frame
fades the existing RGB565 framebuffer toward black by the time-correct
equivalent of a 15% black overlay at 60 Hz, then draws the current particle
heads. A native spatial-dither path preserves the same decay duration when the
Desktop renderer runs faster than RGB565 can represent with a single channel
step. This keeps the trail long and softly graduated without allocating Lua
trail-point arrays.

The animation loop is uncapped. Particle generation is time-based rather than
frame-count-based, so idle density and burst intensity stay consistent across
Desktop and AMOLED frame rates. The AMOLED power key mapping is retained;
Desktop maps Escape to the same back/power component.

Desktop target:

```sh
bazel run --config=macos \
  //projects/example/targets/cc_binary/lua-ferrofluid-intercom:example-lua-ferrofluid-intercom
```

AMOLED package:

```sh
source ../firmware-devenv/export.sh
bazel build --config=esp32s3 \
  //projects/example/targets/h2loader_tar_zlib/lua-ferrofluid-intercom/amoled:package
```

Phase 2 will add Bluetooth discovery, pairing, transport, and remote output
level feeding without changing the visualizer's audio-reactivity boundary.
