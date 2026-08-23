# Allwinner Linux Components

`libs/pal/providers/allwinner-linux/` 保存可被多个 Allwinner Linux board 复用的 vendor SDK integration。通用 Linux userspace PAL 属于 [`libs/pal/providers/linux/`](./linux)；本目录不复制 pthread、framebuffer、netif 或其它 generic Linux provider，也不拥有某块物理板的 GPIO、panel、device path、service unit 或 App 配置。

## 目录结构

```text
libs/pal/providers/allwinner-linux/
└── cedarx_video_decoder/     # T113 CedarX H.264 Video Decoder PAL
```

`cedarx_video_decoder` 只使用 Tina Linux SDK 的 CedarX vdecoder API。Component 从 Video Decoder PAL 接收 H.264 Annex-B packet，使用 `RequestVideoStreamBuffer`/`SubmitVideoStreamData` 提交，完成 `RequestPicture`/`ReturnPicture` 配对、cache coherency 和 allocator-backed linear frame normalization。MP4 demux 位于 `libs/mp4_decoder`；component 不使用 `CdxParser`、路径或 container API。Vendor header、`VideoDecoder` 与 `VideoPicture` 都不能泄漏到 PAL 或 Runtime public header。

K4B 的视频解码由 CedarX VE 硬件完成；解码后的 YUV surface 仍需在 userspace 转换为 Display PAL 的 RGB565 输入。AAC 与音频输出不属于 Allwinner vendor component：AAC-LC 使用 `libs/pal/providers/linux/fdk_aac_decoder` 的固定 source dependency，PCM 播放使用 `libs/pal/providers/linux/alsa_audio`。目标 image 自带的 CedarX AAC binary 不是该合同的一部分。

## Build

Host contract test 使用 fake vendor backend，不证明目标 SDK 或硬件可用：

```sh
bazel test //libs/pal/providers/allwinner-linux/cedarx_video_decoder:cedarx_video_decoder_test
```

对 K4B 交付前还必须使用目标 Tina Linux SDK headers、目标 image libraries 和 ARM hard-float ABI 执行完整 link，并确认 ELF interpreter 是 `/lib/ld-linux-armhf.so.3`。随后在实机验证 H.264 decode、frame content、循环、release、service stop 和持续运行。Host fake、Desktop FFmpeg 或仅编译通过不能替代 K4B 硬件证据。

视频性能验收必须使用 `-c opt` 产物。K4B toolchain 将 Bazel `opt` compilation mode 固定映射为 `-O2 -DNDEBUG`；`fastbuild` 产物只用于编译诊断，不能用于判断帧率或音视频连续性。

K4B vendor build 只接受 `firmwares-devenv` 导出的
`K4B_CEDARX_INCLUDE_DIR` 和 `K4B_CEDARX_LIB_DIR` compact layout；include
目录必须含 `vdecoder.h`/`memoryAdapter.h`，library 目录必须含
`libvdecoder.so`/`libMemAdapter.so`。Component 和 launcher 都不推导旧的
`buildroot/package` source tree。Bazel 通过 `repo_env` 把这两个目录映射为 `@h2_k4b_cedarx_sdk`，vendor headers 和 shared libraries 只作为 external repository input，不进入 GizOS source tree。当前 shared library closure 不依赖 zlib，因此 Bazel graph 不读取 host `-lz`。
