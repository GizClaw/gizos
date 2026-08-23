# Audio System Smoke App

This app runs two continuous audio paths for human inspection:

- Loop `/data/audio/music_loop.ogg` through Opus decode and playback.
- Read microphone frames and write them to a separate loopback playback track.

The shared Smoke App lives under `app/` and only uses PAL audio, PAL filesystem,
PAL task, PAL time, and target-independent Opus APIs. Target entrypoints and
product lifecycle integration are owned by consumers. H2Loader board launchers
continue to supply image confirmation, Runtime assembly, and packaged data.

Expected asset:

```text
projects/example/apps/audio-system/data/audio/music_loop.ogg
```

Expected inspection:

- Looped OGG playback remains active.
- Microphone loopback remains active at the same time.
- The scene keeps running until the consumer launcher requests shutdown.
