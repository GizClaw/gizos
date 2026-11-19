# Garden Guard

A touch lane-defense garden game. Plants hold five lanes of a nine-cell lawn against waves of cartoon garden zombies. The portable App in [`app/`](app/) depends only on Runtime and the Display, Touch and Pref PAL contracts; it renders RGB565 in software and pushes only the dirty row bands each frame. The public contract is [`h2_garden_guard.h`](app/include/h2_garden_guard.h).

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
| Shooter | 100 | Fires peas down its lane while a zombie is ahead of it. |
| Pumpkin | 50 | Tough wall that holds zombies while they chew it; shows cracks as it weakens. |
| Shovel | free | Removes a plant without refund. |
| Frostbell | 125 | Unlocked in the shop for 6 coins. Shoots icy seeds that slow a zombie. |
| Bramble | 75 | Unlocked in the shop for 12 coins. Hurts zombies as they bite. |

Cards recharge after use, shown by a draining shade. Shamblers drag one foot, Shellbacks absorb damage with their salvaged plate first, and Runners take quick strides but break easily. Zombies stop to chew plants in their way. The first zombie through a lane sends its cart across, clearing every zombie in that lane. A second zombie reaching the house in that lane ends the run. Five waves arrive after a 20 s ready time, the last announced as the final wave; clearing them all wins. The result screen shows the score and best score and accepts a tap to play again after one second.

The lawn is viewed obliquely from its front edge: distant rows are narrower and closer together, while upright plants and zombies grow larger toward the viewer. All display shapes keep the lanes running right to left toward the house; taller displays show more courtyard behind the lawn. Planting and drag highlights use the same perspective geometry as the ground. The shovel can also target a plant's raised figure.

## Rendering

The courtyard is drawn once, including the tiled entry, back border and retaining wall. The toolbar and parked carts are cached separately. While playing, the renderer draws plants, zombies, projectiles and moving carts from far lane to near, with a one-byte-per-pixel occlusion map for the parked carts. The scene restores and presents only the horizontal bands and column ranges covered in the current or previous frame. The three RGB565 buffers and depth map together use 7 bytes per pixel (1.03 MiB at 480×320, 2.56 MiB at 480×800). There are no frame-time allocations.

Quiet leaves and blinks update on a 120 ms clock. Growth, bloom, shot recoil, jaw strikes, hits and falls follow gameplay events at the default 40 ms frame period. The planted pose also drives the shovel hit area. An idle title screen presents only its first frame. Its dedicated cover illustration uses a 128-color RGB565 palette and per-row runs in flash. It is decoded one row at a time into a 480-byte stack buffer and needs no extra framebuffer.

The Field Journal and Plant Shop open from the title or result. The journal unlocks a card when that zombie first becomes visible. Every finished match adds its run score to the total once. Plant defeats earn 3 coins per 10 defense points, rounded up, with another 12 for a win with plant defeats. Carts still score, but their kills do not earn coins. Purchases permanently unlock cards while each new run begins with its ordinary sun allowance.

The versioned `profile` blob in the `garden_guard` Pref namespace holds the journal, total score, coins, best and unlocks. The App also reads and preserves the legacy `best` key. The Web target saves in the current browser origin and uses a single writable tab where Web Locks are available. The ESP launchers use their board Runtime Pref. The desktop target and unsupported storage keep progress for the session. Runtime statistics are logged every 10 s as `H2_GARDEN_GUARD_STATS`.
