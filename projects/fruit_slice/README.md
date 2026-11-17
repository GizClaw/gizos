# Fruit Slice

A touch fruit-slicing game. The portable App in [`app/`](app/) depends only on Runtime and the Display, Touch and Pref PAL contracts; it renders RGB565 in software and pushes only the dirty row bands each frame. The public contract is [`h2_fruit_slice.h`](app/include/h2_fruit_slice.h).

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

## Native desktop and mobile builds

```sh
bazel build --config=macos_arm64 //projects/fruit_slice/targets/macos_application/macos:fruit_slice
bazel build --config=ios_sim_arm64 //projects/fruit_slice/targets/ios_application/ios:fruit_slice
ANDROID_HOME="$HOME/Library/Android/sdk" ANDROID_NDK_HOME="$HOME/Library/Android/sdk/ndk/28.2.13676358" bazel build --config=android_arm64 //projects/fruit_slice/targets/android_binary/android:fruit_slice
```

The macOS App bundle and desktop binary share the SDL3 launcher; drag the mouse to slice and press Escape or close the window to exit. Use `--config=macos_x86_64` for an Intel macOS build. The iOS simulator command produces an arm64 simulator package. For a physical iPhone, use `--config=ios_arm64` and select a local development identity with `--ios_signing_cert_name`. Device builds resolve the installed `iOS Team Provisioning Profile: *` profile; it must cover the phone and selected identity. Signing material stays in the local Keychain and profile directories. Android requires SDK Platform 36, Build Tools 36.0.0 and an installed NDK; the command shows the local r28c location.

The mobile entries render at 480x800 logical pixels and aspect-fit the native view, using the same coordinate transform for touch. They pause when inactive and stop their game worker before releasing Runtime, storage and display resources. iOS and Android inject native sandbox Preferences, so best scores survive restarts. Android mounts the owned `H2AndroidPlatform.getView()` from the [Android PAL](../../pal/providers/platforms/android/pal_core/README.md); its entry injects the platform's Display and Touch APIs without implementing Canvas or MotionEvent translation. Shared native Runtime defaults and the iOS pointer adapter live in [`libs/native-host`](libs/native-host/); the portable App has no UIKit, JNI or SDL dependency.

The best score is stored as `best` in the `fruit_slice` Pref namespace. Desktop and macOS inject the existing SQLite Pref backend: macOS uses `~/Library/Application Support/FruitSlice/preferences.sqlite`; Linux uses `$XDG_DATA_HOME/gizclaw/fruit-slice/preferences.sqlite` or `~/.local/share/gizclaw/fruit-slice/preferences.sqlite`. `H2_DESKTOP_STORAGE_ROOT` overrides the desktop storage directory with an absolute path. The backend remains owned until Runtime is destroyed, and startup initializes an empty database before the App's first read. Targets without a Pref implementation retain the score in memory only.
