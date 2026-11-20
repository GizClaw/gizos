# Garden Guard

A touch lane-defense garden game. Plants hold five lanes of a nine-cell lawn against waves of cartoon garden zombies. The portable App in [`app/`](app/) uses Runtime and the Display, Touch, Pref and Audio PAL contracts. It renders RGB565 in software and pushes only the dirty row bands each frame. The public contract is [`h2_garden_guard.h`](app/include/h2_garden_guard.h).

| Entry | Board | Package |
| --- | --- | --- |
| [`targets/h2_loader_tar_zlib/espressif/mosaico`](targets/h2_loader_tar_zlib/espressif/mosaico/) | ESP-Mosaico, 480x480 | `--config=esp32s31 …:package` |
| [`targets/h2_loader_tar_zlib/waveshare/esp32p4_wifi6_touch_lcd_4_3`](targets/h2_loader_tar_zlib/waveshare/esp32p4_wifi6_touch_lcd_4_3/) | Waveshare ESP32-P4 4.3", 480x800 | `--config=esp32p4 …:package` |
| [`targets/h2_loader_tar_zlib/waveshare/esp32s3_touch_amoled_1_8`](targets/h2_loader_tar_zlib/waveshare/esp32s3_touch_amoled_1_8/) | Waveshare AMOLED 1.8", 368x448 | `--config=esp32s3 …:package` |
| [`targets/h2_loader_tar_zlib/jieli/jieli_ac791n_devkit`](targets/h2_loader_tar_zlib/jieli/jieli_ac791n_devkit/) | JieLi AC791N DevKit, 480x320 | `--config=ac791n …:package` |
| [`targets/pkg_tar/web`](targets/pkg_tar/web/) | Browser (WebAssembly), 480x320 | `:garden_guard`, `:serve` |
| [`targets/cc_binary/desktop`](targets/cc_binary/desktop/) | macOS/Linux SDL3 window | `:garden_guard` |

Each firmware image is an H2Loader App: it keeps App commands on the board console and confirms its trial boot after the first frame is presented with Touch open. Install the package through GizMoth like other H2Loader Apps. The AC791N App uses the DevKit display, FT6236 touch, and flash preferences. Its Loader stages updates on the SD card. Build it with the pinned JieLi SDK and Linux x86_64 vendor tools on a supported runner.

```sh
bazel build --config=ac791n //projects/garden_guard/targets/h2_loader_tar_zlib/jieli/jieli_ac791n_devkit:package
```

```sh
bazel test //projects/garden_guard/app:all
bazel run //projects/garden_guard/targets/cc_binary/desktop:garden_guard -- 480x320
bazel run -c opt //projects/garden_guard/targets/pkg_tar/web:serve -- --port 8766
```

The desktop launcher accepts an optional `WIDTHxHEIGHT`; click to tap, and press Escape or close the window to exit. It injects no Pref backend, so its best score lasts for one run.

## Play

Choose **Tap to start** on the cover. During a run, **Menu** pauses the garden. Resume to continue the same run, or choose **Main menu** and confirm to abandon it. The result screen offers both **Play again** and **Main menu**. Sun pays for plants: tap a falling or resting sun, or sweep across several, to collect it before it fades. Pick a card in the toolbar and tap a lawn tile, or drag the card onto the tile, to plant it. Tapping the selected card again cancels the choice.

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

## Audio

The original **Morning Rounds** theme plays during a run. The speaker button in the pause menu mutes both music and effects for the current session. Pause holds the music position and clears pending effects. Leaving a run or starting again resets the score. On the Web target, a trusted interaction unlocks audio and the game falls silent in the background or while another tab holds the profile lock.

The final PCM is synthesized from the first-party score in [`h2_garden_guard_score.c`](app/src/h2_garden_guard_score.c) and nine original cues by Claude Opus 5.5 high in [`h2_garden_guard_sfx.c`](app/src/h2_garden_guard_sfx.c). The 16-bar score at 120 BPM repeats every 32 seconds at 16 kHz. The same C11 integer renderer produces samples for the native, Web and desktop targets. Sound effects use five fixed voices with priority and retrigger limits. `bazel test //projects/garden_guard/app:score_test //projects/garden_guard/app:sfx_test` checks the loop seam, each effect at multiple sample rates and overlapping cues.

The game thread coalesces sound cues in a single atomic word. A separate task owns the audio stream, its reusable frame buffer and its mixer cursor. The WL82 task table places the producer on CPU 1 at priority 11, below the SDK audio server (16), mixer (28) and decoder (30). The existing game entry keeps its priority 10 and its SDK-default affinity. The DAC PAL uses a three-frame PCM ring at 16 kHz mono for this game, with 320 samples per frame. ESP task policy places the producer on core 1 at priority 5 and retains the existing display setup and game policy. The desktop routes playback to PortAudio and Web to the browser's AudioContext. Provider failure leaves gameplay running silently.

## Rendering

The courtyard is drawn once, including the tiled entry, back border and retaining wall. The toolbar and parked carts are cached separately. While playing, the renderer draws plants, zombies, projectiles and moving carts from far lane to near, with a one-byte-per-pixel occlusion map for the parked carts. The scene restores and presents only the horizontal bands and column ranges covered in the current or previous frame. The three RGB565 buffers and depth map together use 7 bytes per pixel (1.03 MiB at 480×320, 2.56 MiB at 480×800). There are no frame-time allocations.

Quiet leaves and blinks update on a 120 ms clock. Growth, bloom, shot recoil, jaw strikes, hits and falls follow gameplay events at the default 40 ms frame period. The planted pose also drives the shovel hit area. An idle title screen presents only its first frame. Its dedicated cover illustration uses a 128-color RGB565 palette and 16-row groups in flash. It is decoded into one 480-byte stack buffer and needs no extra framebuffer.

The Field Journal and Plant Shop open from the title or result. The journal unlocks a card when that zombie first becomes visible. Select a discovered card to inspect its portrait, trait, and counter. Close it or press Escape on Web and desktop to return it to the shelf. The Web game observes the browser’s reduced-motion preference. Every finished match adds its run score to the total once. Plant defeats earn 3 coins per 10 defense points, rounded up, with another 12 for a win with plant defeats. Carts still score, but their kills do not earn coins. Purchases permanently unlock cards while each new run begins with its ordinary sun allowance.

The versioned `profile` blob in the `garden_guard` Pref namespace holds the journal, total score, coins, best and unlocks. The App also reads and preserves the legacy `best` key. The Web target saves in the current browser origin and uses a single writable tab where Web Locks are available. The ESP and JieLi launchers use their board Runtime Pref. The desktop target and unsupported storage keep progress for the session. Runtime statistics are logged every 10 s as `H2_GARDEN_GUARD_STATS`.

## Language

The title and pause menus can switch between English and Simplified Chinese. The selection is a separate `language` preference in the `garden_guard` namespace. The title artwork is shared by both languages, and the software renderer draws the localized title and controls. The Web wrapper follows the game's current selection.

The canvas uses a bitmap subset of Droid Sans Fallback Regular. The browser renders cover, journal, and shop text using Droid Sans Fallback and Droid Sans WOFF subsets at the display resolution. The browser shares the game’s cover artwork and control bounds and reads journal and shop labels from the game renderer. Native targets paint bitmap labels directly into RGB565. The Chinese subsets use [the Android Open Source Project font](https://android.googlesource.com/platform/frameworks/base/+/63ade05d76785975fc3292ca030abbaa1dda8891/data/fonts/DroidSansFallback.ttf) from `android-4.4.4_r2` (`63ade05d76785975fc3292ca030abbaa1dda8891`). Its PostScript name is `DroidSansFallback`, version `2.55b`. The [Apache 2.0 license and attribution](targets/pkg_tar/web/droid-sans-fallback-notice.txt) accompany the derived assets. The original font is only needed to regenerate these files. The canvas generator checks each Chinese code point in the UI against the font's Unicode map and uses a common baseline for the 16-pixel master and the hinted 10-pixel small text.

Use FreeType development headers, Python 3, and `fonttools==4.60.2` to regenerate the subsets from the repository root. The command verifies the source SHA-256 before writing either subset.

```sh
scratch=$(mktemp -d)
curl -fL 'https://android.googlesource.com/platform/frameworks/base/+/63ade05d76785975fc3292ca030abbaa1dda8891/data/fonts/DroidSansFallback.ttf?format=TEXT' \
  | python3 -c 'import base64, sys; sys.stdout.buffer.write(base64.b64decode(sys.stdin.buffer.read()))' \
  > "$scratch/DroidSansFallback.ttf"
curl -fL 'https://android.googlesource.com/platform/frameworks/base/+/63ade05d76785975fc3292ca030abbaa1dda8891/data/fonts/DroidSans.ttf?format=TEXT' \
  | python3 -c 'import base64, sys; sys.stdout.buffer.write(base64.b64decode(sys.stdin.buffer.read()))' \
  > "$scratch/DroidSans.ttf"
cc $(pkg-config --cflags freetype2) projects/garden_guard/tools/generate_han.c \
  $(pkg-config --libs freetype2) -o "$scratch/generate_han"
python3 projects/garden_guard/tools/generate_droid.py \
  "$scratch/DroidSansFallback.ttf" "$scratch/generate_han" "$scratch/DroidSans.ttf"
```

The cover retains its original RGB565 palette and pixels. Each 16-row group starts with a complete row. The following rows use horizontal runs and spans copied in place from the previous row. A Huffman table encodes the tokens. To reproduce the conversion, pass the original row-RLE table from commit `8c48db3a899fe6ee367dd58f253c2188d4540ea0` to `tools/compress_cover.py ORIGINAL_INCLUDE app/src/h2_garden_guard_title.inc`. The tool round-trips every pixel against the input.

The zombie sprites and journal portraits are generated from the original drawing paths. See the [sprite sources](tools/zombie_art/) for the format and regeneration command.

## AC791N plant spans

The AC791N entry can replay mature sunflower and peashooter cels for its 480×320 lawn. The original renderer remains the fallback for unprepared assets, other dimensions, growth, damage and action poses. The game advances the onboard XZ decoder in bounded steps during idle time after presenting a title frame or an early-wave frame. Each completed cell/species block can be used immediately. All 90 blocks share a 4,833,840-byte allocation backed by the board Memory PAL. Allocation and decoding errors disable the optional bank for the run.

The archive contains independent XZ/LZMA2 streams with CRC32, grouped by cell and species. Each cel starts with dirty bands and two RGB565 palettes. Rows encode their relative ordinate followed by runs and a zero terminator. Run headers give a relative x and pixel count. Opaque runs use little-endian RGB565 or a palette index and may repeat a color. Each partial pixel stores ordered `(alpha, color index with last-bit)` pairs. A zero row ordinate byte terminates the cel. The runtime tests current depth before replaying any pixel and blends partial pixels against the current RGB565 buffer.

Regenerate on macOS with Apple Clang, Python 3 and the same liblzma as the checked-in bank. Run `python3 projects/garden_guard/tools/generate_plant_spans.py <scratch-directory>` from the repository root. Copy `plant_spans.xzbank` to the AC791N entry's `assets/` and both `plant_spans_*.inc` files to its `src/`. The generator uses the ordinary plant drawing functions at fixed cell projections and verifies each compressed group round trip.
