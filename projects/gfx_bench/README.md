# Local car renderer evaluation

This is a local ESP-Mosaico / ESP32-S31 visual experiment. It does not select either renderer for production. The evaluation copies under `third_party/jet` and `third_party/tgx` keep the initial experiment independent of production dependencies. If adopted, move the selected upstream to an immutable `MODULE.bazel` dependency with its normal `third_party` overlay.

## What runs on the display

The 480x480 screen shows the 404-triangle Neon Motorworks car against a gradient. Both renderers use the same mesh, camera, RGB565 livery and reflection probe, full-resolution framebuffer and 16-bit depth buffer. They run serially through the existing Display PAL; no half-width, interlaced or checkerboard rendering is enabled. The car rotates slowly while idle. Touch interaction is under hardware investigation; local builds and tests do not establish that the latest input changes work on the panel.

- Drag the car to translate it along the ground and turn toward the movement direction.
- Tap the car to cycle FACTORY (original hard normals), POLISHED (body normals averaged within a 55-degree crease limit) and CHROME (reflection probe on the body).
- Tap outside the car's projected bounding box to change JET/TGX while retaining pose and material.
- The top label reports the selected renderer, look and measured end-to-end FPS. Serial logs separate render and display submission time; render time includes the shared overlay.

Jet uses textured Phong paint; tgx uses textured Gouraud paint with its Phong reflection model. The wheels remain unlit and retain their livery. Windows and CHROME use a shared generated, view-normal-indexed studio reflection probe (matcap), recomputed as the car turns. This is an approximate distant environment appearance, not live scene reflection, ray tracing or a PBR metal material. The common probe makes both engines' texture sampling visible without adding a second scene. Jet and tgx have different lighting equations and handedness; the adapter reflects Jet's X axis and triangle winding to align the poses.

The existing Mosaico display provider sends full-width blocks of eight rows, with a PSRAM shadow and blocking DMA completion. TE synchronization is disabled in the board defaults. End-to-end FPS is consequently affected by display copies, byte swapping and bus transfer as well as the renderer. No measured ESP32-S3 result is evidence of ESP32-S31 performance.

## Local-only car assets

The source model and livery are from [Neon Motorworks](https://github.com/CubeCoders/JetExamples/tree/d132ff6998a2c109b93d09d46aa5ecbbce42585c/esp32-neon-car). Its [asset provenance](https://github.com/CubeCoders/JetExamples/blob/d132ff6998a2c109b93d09d46aa5ecbbce42585c/esp32-neon-car/assets/README.md) states that no separate asset licence was supplied. The MIT licence for example code does not grant rights to the car or livery. No redistribution permission is assumed here: keep the source assets, derived C data, screenshots and firmware local. Do not include them in commits, PRs, releases or shared CI artifacts.

This worktree's Git exclude file ignores `projects/gfx_bench/libs/car_model/local/` and `projects/gfx_bench/local_preview/`. The local directory contains `input/car.obj`, `input/livery-256.png` and the derived `car_model_data.c`. Select these local build inputs explicitly with `--define=gfx_local_car=1`. A clean checkout builds an unavailable-asset descriptor instead, so the build graph and generic gesture tests remain usable without redistributing the model. Running the App without local data reports a clear error before opening the display. `prepare_car.py` is first-party conversion code and contains no model data. It preserves UV seams and corrects the original export's mirrored coordinates/inward normals.

Regenerate local data from the existing inputs:

```bash
python3 -I projects/gfx_bench/tools/prepare_car.py --obj projects/gfx_bench/libs/car_model/local/input/car.obj --livery projects/gfx_bench/libs/car_model/local/input/livery-256.png --output projects/gfx_bench/libs/car_model/local/car_model_data.c
```

## Build and validation

Use the repository-pinned ESP-IDF 6.2 environment; the locally adopted SDK and tools are selected by the GizOS toolbox. Build actions do not flash devices.

```bash
eval "$(bun tools/gizos/src/cli.ts env idf 6.2 --shell sh)"
```

```bash
bazel build --config=esp32s31 --define=gfx_local_car=1 //projects/gfx_bench/targets/h2loader_tar_zlib/gfx_bench/esp_mosaico:package
```

Install the resulting `.update.tar.zlib` as a H2Loader App only after identifying the Mosaico device. The factory image is not an App update and must not overwrite the installed Loader. A bring-up window waits up to 20 seconds for USB (a host can send `GFX_BOOT` to continue immediately). Serial recovery is prepared before Runtime initialization; the App is confirmed only after its first successful display submission. Runtime input polling stays stopped. One App-owned worker samples Touch PAL at an 8 ms interval and queues ordered edges for the main loop; the board provider reads an acknowledged HYN212 report only while the active-low touch IRQ is asserted. The yellow cross and TOUCH counter expose received coordinates without relying on serial logging.

Host-only gesture checks and off-screen renders exercise the same scene and adapters but do not qualify real display or touch behavior:

```bash
bazel test //projects/gfx_bench/libs/scene:scene_test
```

```bash
bazel build --define=gfx_local_car=1 //projects/gfx_bench/tools/preview:preview
```

```bash
bazel-bin/projects/gfx_bench/tools/preview/preview projects/gfx_bench/local_preview
```

Preview writes PPM images for both renderers and three looks. The output directory must already exist. Hardware acceptance requires the actual S31 build boot log, touch interactions, serial timing and an optical observation/video; host images and successful SPI submissions do not establish panel correctness.

## Upstream sources

- Jet: `CubeCoders/Jet` commit `601143f95a1dfaa2ef9f5e700b462538e9aa7dcb`, MIT. The copied core excludes `Example.cpp`, `Sample.cpp` and the sample config. `libs/jet_backend/src/JetConfig.hpp` is the evaluation's configuration. The upstream STL/scratch allocations retain the default C++ allocator; adapter objects and shared buffers use injected Memory PAL.
- tgx: `vindar/tgx` commit `d43f88906ab87f877ea249af2232896b097f15a7`, LGPL-2.1. The copied core omits optional bundled font arrays. `src/Misc.h` has one Picolibc compatibility change: include C++ math/stdlib headers and import `std::abs` into `tgx`. Preserve the licence and review distribution requirements before adoption.

Upstream code retains its original formatting. Build definitions list source files explicitly. No external Arduino/display framework is used; both engines render into memory supplied by this App.
