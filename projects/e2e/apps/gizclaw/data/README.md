# GizClaw E2E audio fixtures

`playback_tone_32s_v1.ogg` is a generated 32-second, mono Ogg Opus tone for the
Device player acceptance case. It contains no recorded speech or music. The
AMOLED launcher uses the immutable Git commit URL, so runtime HTTP download,
Opus decoding and physical Audio PAL output remain part of the test.

Generation command (ffmpeg with libopus):

```sh
ffmpeg -f lavfi -i 'sine=frequency=440:sample_rate=16000:duration=32' \
  -af 'volume=0.15,afade=t=in:d=0.05,afade=t=out:st=31.8:d=0.2' \
  -ac 1 -c:a libopus -b:a 16000 -application audio -map_metadata -1 \
  playback_tone_32s_v1.ogg
```

The committed bytes are authoritative; a later ffmpeg version may produce a
different Ogg stream serial or encoded bytes. Do not overwrite this versioned
fixture when changing the scenario.
