# G99 / GizPocket

G99 owns the GizPocket handheld console's portable Main, product Loader UI, Desktop and Web entries, product regression suite, artwork and firmware packages. Its first hardware target is [ESP-Mosaico / ESP32-S31](../../boards/espressif/mosaico/README.md).

## Ownership

- [Main](apps/main/include/h2_g99.h) receives an initialized Runtime and owns navigation, settings, Wi-Fi interactions, LVGL screens and the built-in Orbit Lua app.
- [Loader](apps/loader/BUILD.bazel) owns the brand and installation UI. Its controller copies service events to one UI task and waits for rendering before returning to the caller, including before launching an image.
- [art](libs/art/BUILD.bazel) owns procedural icons, covers, the wordmark, background and the shared Loader/Main brand layout.
- [screen](libs/screen/include/h2_g99_screen_config.h) owns G99's screen configuration. Device entries inject its Display/Touch wrappers; the public Mosaico BSP retains its native orientation.
- [mosaico](libs/mosaico/BUILD.bazel) adapts entry-supplied input mappings. The Main entry maps AI to HOME, BOOT to SCREEN and the fuel gauge to BATTERY. Only the E2E entry enables virtual UP, DOWN and OK keys.
- [Product E2E](apps/e2e/README.md) drives the same production Main through public APIs. Source tests stay with their implementation.
- [Final entries](targets/) own provider selection, Runtime lifetime, task policy and packaging. The public [Device service](../h2loader/libs/device_service/include/h2_device_service.h) supplies Loader protocol, recovery commands and image confirmation.

## Main behavior

Main uses the logical screen size (`k = min(W, H) / 240`) and gives the display to the Lua host while an app runs. Wheel dragging is damped and limited to one item per gesture, 3.6 items/s and 0.18 item per update; release snaps to an exact detent. The nearest icon retains its glow at half-slot positions and the information panel changes once at snap. Its menu displays five entries; Library and Settings open, while the other entries report that they are unavailable. Library contains Orbit. HOME exits Orbit and returns to Library.

Settings implements Wi-Fi scan/password/connect/save, volume, brightness, screen timeout and About. Preferences use the production `g99` namespace; product regression uses the separate `g99_e2e` namespace. Main's brightness defaults to 100%.

The background is drawn once. Wheel dragging follows the finger and snaps on release; a drag beyond 0.3 of an item advances at least one item. Touch input is filtered at 10 ms intervals, rejoins brief releases within 50 ms, and releases a held contact after 500 ms of failed reads. This compatibility filter remains covered by synthetic clean, empty-frame and failed-read cases. During slow frames, a quick release and new press can still be merged by the 50 ms policy.

The public observation and frame callbacks report navigation, touch dragging and frame timing to tests and launchers. UI code stays on its owning thread. Physical input IDs and screen rotation are selected by the final entry.

Screen-off inside Orbit only sets brightness to zero because the Lua host has no pause/resume event. PAL Display exposes no panel sleep operation. The clock assumes UTC+8.

## Desktop

Build both 480×480 and 320×240 entries:

```sh
bazel build --config=macos_arm64 //projects/g99/targets/cc_binary/main:all
H2_DESKTOP_STORAGE_ROOT=/tmp/g99 bazel-bin/projects/g99/targets/cc_binary/main/g99-mosaico
H2_DESKTOP_STORAGE_ROOT=/tmp/g99 bazel-bin/projects/g99/targets/cc_binary/main/g99-landscape
```

Esc is HOME, Space is SCREEN, Up/Down turn the wheel, Enter is OK, and the mouse supplies touch. Desktop layout files configure the simulated Wi-Fi station. `H2_DESKTOP_STORAGE_ROOT` controls persisted settings. `H2_G99_SCRIPT` accepts comma-separated `<delay_ms>:<command>` steps (`home`, `screen`, `up`, `down`, `ok`, `snap:<name>`, `quit`); `H2_G99_CAPTURE_DIR` receives snapshot BMP files.

## Web

Web runs the same Main with origin-local settings. Mosaico selects the public [Web board and enclosure skin](../../boards/espressif/mosaico/web/README.md); the landscape entry retains a plain logical canvas. The G99 entry maps physical AI/BOOT to HOME/SCREEN and adds virtual Up/Down/Enter keyboard navigation. Device rotation is not applied to the logical Web canvas.

```sh
bazel run --config=macos_arm64 //projects/g99/targets/pkg_tar/web/mosaico:serve
bazel run --config=macos_arm64 //projects/g99/targets/pkg_tar/web/landscape:serve
```

Open the printed URL and press Start. Wi-Fi is unsupported unless the page explicitly configures the Web PAL simulated radio before Start. The page publishes `globalThis.h2G99State` and `H2_G99_WEB` route markers. Stop joins input and App users before destroying Runtime and providers; retained cleanup ownership prevents an unsafe second Start.

## Device packages

All device entries reuse the public Mosaico `h2loader` partition layout. Package identity remains `board=esp_mosaico`, `target=esp32s31`.

| Image | Role | Entry |
| --- | --- | --- |
| `g99-loader` | `h2loader` | [loader](targets/h2_loader_tar_zlib/loader/espressif/mosaico/BUILD.bazel) |
| `g99-main` | `app` | [main](targets/h2_loader_tar_zlib/main/espressif/mosaico/BUILD.bazel) |
| `g99-e2e` | `app` | [e2e](targets/h2_loader_tar_zlib/e2e/espressif/mosaico/BUILD.bazel) |

Use the repository-pinned ESP-IDF 6.2 environment with `IDF_PATH` and `IDF_TOOLS_PATH` exported:

```sh
bazel build --config=esp32s31 \
  //projects/g99/targets/h2_loader_tar_zlib/loader/espressif/mosaico:package \
  //projects/g99/targets/h2_loader_tar_zlib/main/espressif/mosaico:package \
  //projects/g99/targets/h2_loader_tar_zlib/e2e/espressif/mosaico:package
```

G99's current device configuration rotates content 90° counter-clockwise and applies the inverse transform to touch. The wrapper uses an allocator-provided RGB565 frame and coalesces two-dimensional dirty regions. After a complete frame establishes the panel contents, it excludes unchanged pixels from subsequent transfers. The BSP expands them only to the panel's four-column/eight-row boundaries and packs them into DMA batches. This policy belongs only to G99. All three entries use its single configuration header.

Loader shows the brand alone at power-on, installation state/progress and failures, then Starting after an install. UI failure is reported while the command service remains available. It uses one UI worker for initialization, all drawing and teardown. Main and E2E retain USB recovery commands for their boot lifetime. Main confirms once its menu and input are ready; E2E confirms a completed run, including a completed run with failing cases, and reports the failures separately.

App packages carry the pinned font's OFL license under `/data/licenses`; E2E also carries `/data/e2e/select.txt`. Fonts are build-time subsets of the pinned Noto Sans SC Bold 2.004 input.

## Validation

```sh
bazel test --config=macos_arm64 //projects/g99/...
bazel test --config=macos_arm64 //projects/g99/targets/cc_test/e2e:all
bazel test --config=macos_arm64 //projects/g99/targets/pkg_tar/web/e2e/...
```

Desktop and Web tests cover both logical sizes without devices or external service credentials. Loader tests cover rendered states, serialized submissions from concurrent callers, render failures and retained ownership after a failed join. Screen tests cover rotation, unrotated passthrough, touch coordinates and allocation failure.

The device frame budget requires at least 20 FPS (50 ms per active animation frame), including Library/Settings entry and exit. Only the device E2E entry declares `frame-budget`; shared-host performance cases report observations without enforcing hardware budgets. Device Wi-Fi cases require the documented test AP. Building a package or passing host/browser tests does not establish physical-device acceptance. Device acceptance requires the on-board E2E plus a separate physical drag and AI return check in the interactive Main image.
