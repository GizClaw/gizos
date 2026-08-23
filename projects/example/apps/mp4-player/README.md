# MP4 Player Smoke App

`mp4-player` is a shared Smoke App that exercises `libs/mp4_decoder`, packet-fed Video/Audio Decoder PAL providers, Audio PAL, and Display PAL with deterministic launcher-selected MP4 assets. The App implements looping with `seek(0)` and separates decode, audio output, and video presentation into three execution contexts. The blocking caller remains the video presentation context so Desktop keeps SDL event polling and framebuffer presentation on its calling thread; decoder and audio output run in two Runtime tasks.

The decoder task copies each borrowed MP4 presentation frame into one of three reusable slots. Each slot carries RGB565 pixels, the PCM interval for the same PTS, and a consumer count. The audio writer task and caller-owned video presentation loop receive the slot index through separate queues; the slot returns to the free queue only after both consumers release it. Display or Audio PAL backpressure can therefore overlap decoding of the next frame instead of serializing every operation in one loop.

The three-slot pipeline absorbs bounded output jitter but does not increase decoder throughput. Each target asset must still keep its native Video Decoder provider below the presentation deadline.

## Test Asset

The Bazel package keeps portable App ownership separate from media ownership. `:mp4_player` contains the App and its documentation, while `:large_media` contains only the 1024×600 sample. Every launcher that consumes this shared asset depends on the portable target plus the matching media target; H2Loader launchers with target-native media keep that media under their own launcher root.

`data/media/test_1024x600_h264_aac.mp4` retains the stable MP4Player resource path and uses the same H.264 video stream as the Showcase Big Buck Bunny sample. Its embedded audio track is encoded as 16 kHz mono AAC-LC to match the Audio PAL playback format. The 20-second sample exercises decoded video, decoded audio, synchronized presentation, and looping with product-visible footage instead of a generated test pattern.

| Property | Value |
| --- | --- |
| Video | H.264 High, level 3.1, 1024×600, 24 fps |
| Audio | AAC-LC, mono, 16 kHz |
| Duration | 20 seconds |
| SHA-256 | `2a59d2e5885afc0d5b036ea8ccd466ebca353f4c19fcbb2a6192441896c268f1` |

Big Buck Bunny is © 2008 Blender Foundation and licensed under Creative Commons Attribution 3.0. The container metadata identifies the Peach Open Movie Team and preserves the original title, artist, composer, date, and copyright attribution. The H.264 packet stream remains byte-identical to the Showcase source; only the audio track is conformed to the PAL playback format.

## Android Mobile Launcher

Android provides one smoke launcher at `projects/example/targets/android_binary/mp4-player`. It packages `:large_media`, presents it on a 1024×600 logical surface, decodes High Profile H.264 and raw AAC-LC through NDK MediaCodec, and plays the 16 kHz mono track through AAudio. Microphone capture and production audio lifecycle policy remain unsupported.

## Target Images

The P4 and BK7258 launchers package target-native six-second derivatives of the same sample:

| Target | Video | Audio | SHA-256 |
| --- | --- | --- | --- |
| Waveshare P4 | H.264 Constrained Baseline, 480×800 native scan with pre-rotated landscape content, 6 fps, 1 reference frame | AAC-LC, mono, 16 kHz | `e9fc56908ae50b6abfae67507f3d2edeb12d3b4ef9bee271378245b7a2f1590a` |
| BK7258 | H.264 Constrained Baseline, 800×480, 15 fps, 1 reference frame | AAC-LC, mono, 16 kHz | `436ebc71592c5c4c4d1962a0532a4852e5336ca3028ac92b5406b634ad44dc96` |

The launcher passes `/data/media/showcase.mp4` to the portable App through Runtime FS. Both images use PSRAM for MP4 state and decoded buffers, confirm the H2Loader image after the first presented frame, and then loop continuously. The one-reference-frame encoding bounds TinyH264's decoded-picture buffer on memory-constrained targets.

Verify the committed hash from the App root:

```sh
cd data/media
shasum -a 256 -c SHA256SUMS
```
