# Showcase Assets

`media/big_buck_bunny_showcase.mp4` is a 20-second 1024×600 H.264/AAC Showcase fallback derived from the Blender Foundation open movie *Big Buck Bunny*. The matching `media/big_buck_bunny_showcase_s16le_16k_mono.pcm` contains the same segment's audio converted to signed 16-bit, 16 kHz, mono PCM for Audio PAL playback.

Source: `BigBuckBunny_640x360.m4v` from the [official Blender download directory](https://download.blender.org/peach/bigbuckbunny_movies/).

*Big Buck Bunny* is copyright Blender Foundation and released under [Creative Commons Attribution 3.0](https://creativecommons.org/licenses/by/3.0/). The segment is resized, cropped, normalized, faded at its audio loop boundary, and transcoded for this package.

`fonts/NotoSansSC-Bold.ttf` is the Showcase-owned copy of the complete Chinese UI font imported from the confirmed H106 source asset. Runtime code uses the complete font with a bounded TinyTTF glyph cache; it does not use the Desktop status-surface subset or read the H106 asset path.
