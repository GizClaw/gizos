# Garden Guard

A touch lane-defense garden game. Plants hold five lanes of a nine-cell lawn against waves of garden pests. The portable App in [`app/`](app/) depends only on Runtime and the Display, Touch and Pref PAL contracts; it renders RGB565 in software and pushes only the dirty row bands each frame. The public contract is [`h2_garden_guard.h`](app/include/h2_garden_guard.h).

| Entry | Board | Package |
| --- | --- | --- |
| [`targets/h2_loader_tar_zlib/espressif/mosaico`](targets/h2_loader_tar_zlib/espressif/mosaico/) | ESP-Mosaico, 480x480 | `--config=esp32s31 …:package` |
| [`targets/h2_loader_tar_zlib/waveshare/esp32p4_wifi6_touch_lcd_4_3`](targets/h2_loader_tar_zlib/waveshare/esp32p4_wifi6_touch_lcd_4_3/) | Waveshare ESP32-P4 4.3", 480x800 | `--config=esp32p4 …:package` |
| [`targets/h2_loader_tar_zlib/waveshare/esp32s3_touch_amoled_1_8`](targets/h2_loader_tar_zlib/waveshare/esp32s3_touch_amoled_1_8/) | Waveshare AMOLED 1.8", 368x448 | `--config=esp32s3 …:package` |
| [`targets/pkg_tar/web`](targets/pkg_tar/web/) | Browser (WebAssembly), 480x320 | `:garden_guard`, `:serve` |
| [`targets/cc_binary/desktop`](targets/cc_binary/desktop/) | macOS/Linux SDL3 window | `:garden_guard` |

Each firmware image is an H2Loader App: it keeps App commands on the board console and confirms its trial boot after the first frame is presented with Touch open. Install the package through GizMoth like other H2Loader Apps.

```sh
bazel test //projects/garden_guard/app:all
bazel run //projects/garden_guard/targets/cc_binary/desktop:garden_guard -- 480x320
bazel run -c opt //projects/garden_guard/targets/pkg_tar/web:serve -- --port 8766
```

The desktop launcher accepts an optional `WIDTHxHEIGHT`; click to tap, and press Escape or close the window to exit. It injects no Pref backend, so its best score lasts for one run.

## Play

Tap the screen to start. Sun pays for plants: tap a falling or resting sun, or sweep across several, to collect it before it fades. Pick a card in the toolbar and tap a lawn tile, or drag the card onto the tile, to plant it. Tapping the selected card again cancels the choice.

| Card | Cost | Role |
| --- | --- | --- |
| Sunflower | 50 | Drops a sun every 12 s. |
| Shooter | 100 | Fires peas down its lane while a pest is ahead of it. |
| Pumpkin | 50 | Tough wall that holds pests while they chew it; shows cracks as it weakens. |
| Shovel | free | Removes a plant without refund. |

Cards recharge after use, shown by a draining shade. Beetles walk, shell-backed beetles absorb damage with their plate first, and red ants run fast but break easily. Pests stop to chew any plant in their way. Each lane has one cart at the house: the first pest through sends it down the lane, clearing every pest in it. A second pest reaching the house in that lane ends the run. Five waves arrive after a 20 s ready time, the last announced as the final wave; clearing them all wins. The result screen shows the score and best score and accepts a tap to play again after one second.

Landscape displays run the lanes from right to left toward the house on the left; portrait displays run them from top to bottom toward the house at the bottom, so the same game fits 480x320, 480x480, 480x800 and 368x448 panels.

## Rendering

The background (toolbar wood, lawn checkerboard, stone path) is drawn once. The static layer has two independently keyed parts: the toolbar (sun, cards, cooldowns, wave progress) and the field (plants, parked carts, title or result panel). A part is repainted only when its key changes. Pests, peas, suns, particles and banners form the moving scene, whose covered bands in this and the previous frame are restored and pushed. An idle title screen pushes nothing after its first frame. The default frame period is 40 ms.

The best score is stored as `best` in the `garden_guard` Pref namespace. Targets without a Pref implementation retain it in memory only. Runtime statistics are logged every 10 s as `H2_GARDEN_GUARD_STATS`.
