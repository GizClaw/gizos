# ESP-SR model artifacts

This host tool writes ESP-SR's binary index with explicit little-endian fields and aligned data views. The `nihaoxiaozhi` artifact contains the immutable upstream `wn9_nihaoxiaozhi_tts` model (“你好小智”). Input files are downloaded using the revision and SHA-256 values in `MODULE.bazel`; unresolved Git LFS pointers, empty data and oversized models fail before publication. No model weights are checked into GizOS.

The model remains subject to the upstream [ESP-SR License](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/LICENSE). This tool changes the container and alignment, not the neural parameters or wake phrase. The SDK-coupled loader belongs to `native_component_src/esp-idf6.x/h2_wakenet/`; each consumer owns its package path and local wake behavior.

```sh
bazel test --config=macos_arm64 //tools/esp_sr_model:pack_test
bazel build --config=macos_arm64 //tools/esp_sr_model:nihaoxiaozhi
```
