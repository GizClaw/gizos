# G99 / GizPocket

G99 owns the GizPocket handheld console's portable Main, product Loader UI, Desktop and Web entries, product regression suite, artwork and firmware packages. Its first hardware target is [ESP-Mosaico / ESP32-S31](../../boards/espressif/mosaico/README.md).

## Ownership

- [Main](apps/main/include/h2_g99.h) receives an initialized Runtime and owns navigation, settings, Wi-Fi interactions, LVGL screens and the built-in Orbit Lua app.
- [Loader](apps/loader/BUILD.bazel) owns the brand and installation UI. Its controller copies service events to one UI task and waits for rendering before returning to the caller, including before launching an image.
- [art](libs/art/BUILD.bazel) owns procedural icons, covers, the wordmark, background and the shared Loader/Main brand layout.
- [screen](libs/screen/include/h2_g99_screen_config.h) owns G99's screen configuration. Device entries inject its Display/Touch wrappers; the public Mosaico BSP retains its native orientation.
- [mosaico](libs/mosaico/BUILD.bazel) adapts entry-supplied input mappings. The Main entry maps AI to HOME, BOOT to SCREEN, the fuel gauge to BATTERY and the existing PWM motor to HAPTICS. Only the E2E entry enables virtual UP, DOWN and OK keys.
- [Product E2E](apps/e2e/README.md) drives the same production Main through public APIs. Source tests stay with their implementation.
- [Final entries](targets/) own provider selection, Runtime lifetime, task policy and packaging. The public [Device service](../h2loader/libs/device_service/include/h2_device_service.h) supplies Loader protocol, recovery commands and image confirmation.

## Main behavior

Main uses the logical screen size (`k = min(W, H) / 240`) and gives the display to the Lua host while an app runs. Wheel dragging is damped and limited to one item per gesture, 3.6 items/s and 0.18 item per update; release snaps to an exact detent. The nearest icon retains its glow at half-slot positions and the information panel changes once at snap. Its menu displays five entries; Library and Settings open, while the other entries report that they are unavailable. Library contains Orbit. HOME exits Orbit and returns to Library.

The first menu entry is Circle (`圈子`), with a grouped-people icon. Its stable numeric identifier is unchanged. QR discovery, Bluetooth friendship and game sharing remain unavailable.

Library presents a real Orbit screenshot across the left preview area, cropped without stretching and faded into the information panel on its right edge. The right side retains the game title, author, description and Start action; the Library counter and implementation metadata are omitted. A three-dot button below the title, beside the author, opens a right-side management drawer. The drawer only lists executable actions: an internally installed game can move outward, an externally installed game can move inward, and uninstall requires its own capability. Orbit is embedded in firmware and has no management actions, so its drawer shows an explanation instead of unavailable options. Outside tap, Close and HOME dismiss the drawer; background launch/navigation is blocked while it is present, and screen-off dismisses it. Future installable Lua packages will supply their own game artwork and management capabilities.

Settings implements Wi-Fi scan/password/connect/save, volume, brightness, screen timeout, About, vibration feedback and storage usage. Its wheel omits Input and represents Language with a capital A; published setting identifiers retain their values. The main menu centers its title and supporting status in the information panel, with a larger title-to-detail gap and at most two lines of supporting text. Preferences use the production `g99` namespace; product regression uses the separate `g99_e2e` namespace. Main's brightness defaults to 100%.

Settings pages share centered headings and consistent content spacing. About measures each row's wrapped value before placing the next row, with a smaller text fallback at 320×240. Wi-Fi shows only status while empty, scanning without results, or unsupported; its list selection and Connect button fade in over 240 ms after results exist. An in-progress pointer gesture keeps its widget and list layout stable.

Vibration feedback is a direct on/off switch, defaults off, and persists across restarts. A mapped PWM motor supplies 60 ms feedback at 20% duty on accepted navigation and selection actions and when enabling the switch. Disabling it, turning off the display, and App shutdown stop the pulse. Environments without a motor retain the preference and identify that limitation on the page.

Storage queries run on a separate App worker while the Storage page is visible, refreshing every five seconds. The final entry injects the [read-only storage contract](apps/main/include/h2_g99_storage.h); Main contains no native SDK or board dependencies. Mosaico reports internal image bytes, allocated LittleFS blocks and reserved metadata against physical Flash capacity; external NAND reports mapped logical bytes against usable Dhara FTL capacity. The native query neither formats nor writes storage. Future BK7258 entries can inject external SPI Flash statistics through the same contract; no BK7258 board implementation is implied. Web storage values are explicitly labeled preview data.

Each storage medium uses a rectangular progress frame with its name and used/total values inside; the filled area represents the used fraction.

Volume and brightness sliders follow continuous pointer drags in one-percent increments, keep the active widget until release and save once the gesture ends. Percentage values are centered vertically in the information panel, with the slider below and no explanatory caption. Background taps leave these direct controls unchanged, and their values keep the same text styling without a click-to-edit confirmation step. Releasing the slider ends its value editor so the next wheel gesture can navigate immediately. A wheel drag can also leave an active volume or brightness key editor; key-based UP/DOWN adjustments retain their ten-percent steps. Settings omits the item counter and on-screen key hints.

Screen timeout is a bounded vertical wheel in the right information panel, with the same damping and finite magnetic snap as the Wi-Fi list. Releasing a drag or tapping a row immediately applies and saves the centered choice; there is no checkmark or confirmation step. The left settings wheel remains independently usable, and each gesture retains the side where it started even after crossing the divider. Keyboard focus changes also apply immediately; OK only leaves key adjustment.

The background is drawn once. Vertical dragging on the screen background turns the current wheel and snaps on release; a drag beyond 0.3 of an item advances at least one item. Outside the direct volume, brightness and screen-timeout pages, a tap on the background confirms the current selection, including above or below the wheel centre and on the information panel. Buttons, sliders, right-side lists and the keyboard retain their own touch actions; background taps leave an active editor unchanged. Touch input is filtered at 10 ms intervals, rejoins brief releases within 50 ms, and releases a held contact after 500 ms of failed reads. An expired release completes before the next contact is read, even after a slow frame; a new DOWN without an intervening UP closes the old gesture before starting from the new contact's coordinates. Source tests cover these boundaries and the compatibility models, while product E2E checks that two upward drags separated by a missing release advance twice and finish at exact detents. Gesture logs include both endpoints and elapsed time.

The public observation and frame callbacks report navigation, touch dragging and frame timing to tests and launchers. Library's left selection rectangle represents the screenshot area. UI code stays on its owning thread. Physical input IDs and screen rotation are selected by the final entry.

Settings > Wi-Fi has two independent gesture regions split at the information panel's left edge. A press on the left controls the settings wheel, including while a network is selected; leaving Wi-Fi ends that selection editor. A press on the right controls the bounded vertical network wheel, follows with damping at up to eight rows/second and snaps to the nearest row with the same 220 ms finite spring as the settings wheel. Crossing the divider during a press keeps its original owner. Tapping a row selects it; the Connect button or OK confirms it and opens password entry when needed. Scanned networks are applied after an active list gesture or snap finishes, and the selected SSID is preserved when scan ordering changes.

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

Web runs the same Main with origin-local settings. Mosaico selects the public [Web board and enclosure skin](../../boards/espressif/mosaico/web/README.md); the landscape entry retains a plain logical canvas. G99's [Mosaico CSS](targets/pkg_tar/web/mosaico.css) presents the enclosure in its product orientation with BOOT/POWER on the left and AI on the right; the preview omits the camera. It counter-rotates the screen container so the logical canvas and pointer coordinates stay upright. The G99 entry maps physical AI/BOOT to HOME/SCREEN and adds virtual Up/Down/Enter keyboard navigation.

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
