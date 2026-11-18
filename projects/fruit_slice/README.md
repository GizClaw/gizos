# Fruit Slice

A touch fruit-slicing game. The portable App in [`app/`](app/) depends only on Runtime and the Display, Touch, Pref and optional Audio PAL contracts; it renders RGB565 in software and pushes only the dirty row bands each frame. The public contract is [`h2_fruit_slice.h`](app/include/h2_fruit_slice.h).

| Entry | Board | Package |
| --- | --- | --- |
| [`targets/h2_loader_tar_zlib/espressif/mosaico`](targets/h2_loader_tar_zlib/espressif/mosaico/) | ESP-Mosaico, 480x480 | `--config=esp32s31 …:package` |
| [`targets/h2_loader_tar_zlib/waveshare/esp32p4_wifi6_touch_lcd_4_3`](targets/h2_loader_tar_zlib/waveshare/esp32p4_wifi6_touch_lcd_4_3/) | Waveshare ESP32-P4 4.3", 480x800 | `--config=esp32p4 …:package` |
| [`targets/h2_loader_tar_zlib/waveshare/esp32s3_touch_amoled_1_8`](targets/h2_loader_tar_zlib/waveshare/esp32s3_touch_amoled_1_8/) | Waveshare AMOLED 1.8", 368x448 | `--config=esp32s3 …:package` |
| [`targets/h2_loader_tar_zlib/beken/bk7258_v3_202405`](targets/h2_loader_tar_zlib/beken/bk7258_v3_202405/) | BK7258 DevKit V3 | `--config=bk7258 …:package` |
| [`targets/h2_loader_tar_zlib/jieli/jieli_ac791n_devkit`](targets/h2_loader_tar_zlib/jieli/jieli_ac791n_devkit/) | JieLi AC791N DevKit, 480x320 | `--config=ac791n …:package` |
| [`targets/pkg_tar/web`](targets/pkg_tar/web/) | Browser (WebAssembly) | `:fruit_slice`, `:serve` |
| [`targets/cc_binary/desktop`](targets/cc_binary/desktop/) | macOS/Linux SDL3 window | `:fruit_slice` |
| [`targets/macos_application/macos`](targets/macos_application/macos/) | macOS 13+ App bundle | `--config=macos_arm64 …:fruit_slice` |
| [`targets/ios_application/ios`](targets/ios_application/ios/) | iPhone/iPad, iOS 16+ | `--config=ios_sim_arm64 …:fruit_slice` |
| [`targets/android_binary/android`](targets/android_binary/android/) | Android 12+, arm64 | `--config=android_arm64 …:fruit_slice` |

Each firmware image is an H2Loader App: it keeps App commands on the board console and confirms its trial boot after the first frame is presented with Touch open.

The AC791N target requires the pinned JieLi SDK and Linux x86_64 compiler/post-build tools. Build it on Linux x86_64 or in an x86_64 environment on macOS, then install the resulting package through GizMoth. Its version combines the Fruit Slice App version with `ac791n-devkit` and the board build counter; ordinary builds omit `variant`, which is reserved for build variants such as `prod` or `e2e`.

```sh
bazel test //projects/fruit_slice/app:all
bazel run -c opt //projects/fruit_slice/targets/pkg_tar/web:serve -- --port 8765
bazel run //projects/fruit_slice/targets/cc_binary/desktop:fruit_slice
```

Bombs use a fading, expanding flash around the impact point so the effect does not blend and retransmit the full screen on every frame. Dirty row bands keep their own column spans; only adjacent equal spans are merged for display submission. Runtime statistics include `blast_frames` and `blast_max_ms` for frames with an active bomb flash.

## Coins and weapons

Tap **Start Game** on the title screen or slice its melon to begin. After a run and any card reveals, tap **Play Again** or slice the orange to retry. The three equal footer buttons open Tasks, Cards and Shop; their drawn bounds are also their touch targets. The play HUD keeps the score on the left, remaining hearts on the right (a gray cross marks a lost life), and landed cards in the center, leaving the fruit field clear.

Scores still measure each run. Tap **Tasks** on the title or game-over screen to claim a separate coin currency: slicing 10 fruit earns 15 coins, making three combos earns 25 coins, and reaching 20 points in one run earns a one-time 40 coins. A combo means slicing at least three fruit in one swipe. The first two tasks repeat by consuming earned progress on each claim; extra progress carries over between runs and claims. Menu fruit never count. An existing best of at least 20 unlocks the score task on upgrade.

Tap **Shop** to buy and immediately equip the Jade Katana (40 coins) or Flame Axe (90 coins), or switch back to the free Classic Knife. Owned weapons can be equipped again for free. The katana adds 10 pixels of cut reach and a green trail; the axe adds 20 pixels and a wider orange trail, measured at a 480-pixel short side and scaled with the display. Wider cuts also hit nearby bombs. Shop and task pages pause the scene; tap **Back** to return, then tap **Start Game** / **Play Again** or slice the menu fruit to play.

The `progress_v1` Pref blob in the `fruit_slice` namespace stores coins, unclaimed task progress, the score reward's claimed state, owned weapons and equipped weapon together using an explicit versioned little-endian format. Claims, purchases and equipment changes request a save at the end of the frame; ordinary task progress is saved at most every two seconds and on orderly exit. Failed writes retain the pending state for retry and show **Save failed - retrying** on these pages. Platforms without Pref support show **Session only - no storage** and keep progress for the current App session. Missing progress starts with zero coins and the Classic Knife, preserving the separate existing `best` score. An unreadable, malformed or future-version profile remains untouched: the game uses session progress and shows **Save unreadable - reopen** until restarted. Currency and unclaimed counters cap at 999999; a full wallet must spend coins before claiming another reward.

## Cards

Tap **Cards** on the title or result screen to open the 16-card album. Its Fruit, Weapon, Bomb and Skill tabs show owned counts, rarity, collection progress and unlock hints. Tap a card to inspect it or swipe horizontally to change tabs; **Back** returns to the screen you came from. Unseen cards have a marker, and opening the album selects the first unseen card.

Cards come from play: five fruit drops, slicing 30 fruit in a run, scoring 10 with each of the three weapons, hitting a bomb, dodging three or eight bombs in a run, making three- or five-fruit combos, and scoring 30 or 60. New common fruit cards have a 12% drop chance per matching slice, rare fruit cards 6%, and a missing fruit card is guaranteed by the twentieth matching slice in the current App session. Owned fruit cards drop at 3%. Each card awards at most one copy per run, with duplicates capped at 99 and no currency or gameplay bonus.

Drops fly into the HUD with glitter while play continues. After a run, first copies reveal from common to legendary with a zoom, charge, flip, flash, rings and rarity rays. Tap once to speed up a flip, then tap again after the face is visible to continue. The album has staggered tab entrances, a floating selection and rarity shimmer. Pools are bounded to four flights and 48 sparks; fixed-size fruit textures are prepared at startup and the effects allocate no frame-time textures.

Collection counts and unseen markers live in the separate 32-byte `fruit_slice/cards_v1` blob, saved at most every two seconds and on orderly exit. Fresh storage is initialized before reading. Temporary read failures retry on the same two-second schedule; a valid stored collection is merged exactly once with session awards before any write, preserving locally inspected cards and capping copies at 99. A missing key permits saving the session collection directly. Failed writes retain the merged dirty state without adding the stored copies again. Active reveals finish normally while pending duplicate reveals are removed. Malformed and future-version collections remain protected; continuing read failures never permit an overwrite. The album reports **Unsaved - retrying**, **Unsaved - protected**, or session-only storage as appropriate. Existing best scores, coin progress, weapon ownership and audio preferences keep their independent keys.

## Sound

The title screen has independent SFX and BGM mute buttons. Settings persist as `fruit_slice/audio_v1`, and playback starts on the first touch. The bank includes slicing, combos with a synthetic nonverbal cheer, explosions, missed fruit, results, economy/card confirmations and an original arcade music loop. See the [sound bank](app/resources/README.md) for the rebuild command, resource license, event integration and limits. Existing Audio PAL providers power desktop, Web, mobile and supported boards; backgrounding or exiting closes game playback.

## Native desktop and mobile builds

```sh
bazel build --config=macos_arm64 //projects/fruit_slice/targets/macos_application/macos:fruit_slice
bazel build --config=ios_sim_arm64 //projects/fruit_slice/targets/ios_application/ios:fruit_slice
ANDROID_HOME="$HOME/Library/Android/sdk" ANDROID_NDK_HOME="$HOME/Library/Android/sdk/ndk/28.2.13676358" bazel build --config=android_arm64 //projects/fruit_slice/targets/android_binary/android:fruit_slice
```

The macOS App bundle and desktop binary share the SDL3 launcher; drag the mouse to slice and press Escape or close the window to exit. Use `--config=macos_x86_64` for an Intel macOS build. The iOS simulator command produces an arm64 simulator package. For a physical iPhone, use `--config=ios_arm64` and select a local development identity with `--ios_signing_cert_name`. Device builds resolve the installed `iOS Team Provisioning Profile: *` profile; it must cover the phone and selected identity. Signing material stays in the local Keychain and profile directories. Android requires SDK Platform 36, Build Tools 36.0.0 and an installed NDK; the command shows the local r28c location.

The mobile entries render at 480x800 logical pixels and aspect-fit the native view, using the same coordinate transform for touch. They pause when inactive and stop their game worker before releasing Runtime, storage and display resources. iOS and Android inject native sandbox Preferences, so best scores, coins, task rewards and weapons survive restarts. Android mounts the owned `H2AndroidPlatform.getView()` from the [Android PAL](../../pal/providers/platforms/android/pal_core/README.md); its entry injects the platform's Display and Touch APIs without implementing Canvas or MotionEvent translation. Shared native Runtime defaults and the iOS pointer adapter live in [`libs/native-host`](libs/native-host/); the portable App has no UIKit, JNI or SDL dependency.

The best score is stored as `best` in the `fruit_slice` Pref namespace. Desktop and macOS inject the existing SQLite Pref backend: macOS uses `~/Library/Application Support/FruitSlice/preferences.sqlite`; Linux uses `$XDG_DATA_HOME/gizclaw/fruit-slice/preferences.sqlite` or `~/.local/share/gizclaw/fruit-slice/preferences.sqlite`. `H2_DESKTOP_STORAGE_ROOT` overrides the desktop storage directory with an absolute path. The backend remains owned until Runtime is destroyed, and startup initializes an empty database before the App's first read. Targets without a Pref implementation retain best scores and progression in memory only.
