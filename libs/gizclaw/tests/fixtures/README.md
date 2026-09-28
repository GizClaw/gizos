# GizClaw audio decoder MP3 fixtures

Each file is LAME 3.100 output from a generated 1 kHz sine at 16384 peak
(-6 dBFS), 44.1 kHz PCM16; stereo sources carry the sine on the left channel
and silence on the right, so a mono mix-down is a half-amplitude sine.

| File | Source | LAME options | Exercises |
| --- | --- | --- | --- |
| `tone_1k_44k_stereo_cbr64.mp3` | 2.0 s stereo | `-b 64 --cbr -m j --tt "Tone" --add-id3v2` | ID3v2 tag, LAME "Info" (CBR) tag with encoder delay and padding, MPEG-1 joint stereo |
| `tone_1k_22k_mono_vbr.mp3` | 3.0 s mono | `-V 6 -m m --resample 22.05` | "Xing" (VBR) tag with frame count and TOC, MPEG-2 |
| `tone_1k_8k_mono_cbr8_notag.mp3` | 1.0 s mono | `-b 8 --cbr -m m --resample 8 -t` | No tag at all, MPEG-2.5 |

The sources can be regenerated with Python's `wave` module writing
`int(16384 * sin(2 * pi * 1000 * i / 44100))` per sample. `SHA256SUMS`
pins the committed bytes; do not overwrite a fixture in place, add a new one.
