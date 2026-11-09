# Original AAC-LC fixtures

These are original generated sine waves, encoded by the command in
`generate_vectors.py`. No third-party song or media is used.

Encoder: `ffmpeg version 8.1.1 Copyright (c) 2000-2026 the FFmpeg developers`. The committed bytes are the stable test input;
regeneration with a different encoder version may change them.

- `mono`: 44100 Hz, 1 channel(s), tones [997] Hz, 13 raw AAC packets, ASC `1208`, ADTS SHA-256 `12fb30314dfb211f23c26e641ef5fa2822b05906ad00fb71f90ed3b23f133c20`.
- `stereo`: 48000 Hz, 2 channel(s), tones [997, 1999] Hz, 13 raw AAC packets, ASC `1190`, ADTS SHA-256 `ee6e0a3a6b70626f49312ad7c9d2e47ef88a40137799203641685a7d7d17c2f4`.

ADTS framing is removed before embedding. The PAL receives raw AAC-LC
access units and a separate AudioSpecificConfig. PCM checks allow codec
rounding and encoder priming while requiring nonzero signal, correct
format, channel separation and the original tone frequencies.

Generated C SHA-256: `1c9597bbccb8bfb188ee9489dc77b9e0648ade3fefa7528260c2c1515bf855a3`.
