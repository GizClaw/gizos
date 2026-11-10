# Desktop recording

This public host package writes desktop display and speaker capture to a bounded, streaming MPEG-4 Visual/AAC MP4 file on Linux and macOS. It uses native workers and FFmpeg; it does not open devices or own capture registration.

External consumers depend on `@gizos//tools/desktop_recording` and include `h2_desktop_recording.h`. This replaces the removed `@gizos//libs/desktop_recording` label while preserving the C API and statistics fields.

Create a recorder with `h2_desktop_recording_create`, pass its borrowed hooks to `h2_desktop_capture_create`, and poll `h2_desktop_recording_result` for capture or output failures. Unregister capture before calling `h2_desktop_recording_stop` and `h2_desktop_recording_destroy`. The output file must not already exist. See the [public header](include/h2_desktop_recording.h) for buffer limits, timestamp semantics and lifetime requirements.

Run the encoder and output regression tests with:

```sh
bazel test //tools/desktop_recording:encoder_test
```

The test feeds synthetic capture frames, reads the resulting MP4 streams and decoded audio, and exercises bounded buffering, finalization, output stalls and I/O failures without devices. The implementation is in `src/`; production capture registration remains in `pal/providers/platforms/desktop/app_support`.
