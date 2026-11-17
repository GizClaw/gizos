# QR codes

The default `:qrcode` encoder and RGB565 rasterizer use caller-owned buffers. The separate `:decoder` target reads an injected 8-bit luma plane, including the Y plane of NV12, and returns one raw payload without a NUL terminator.

[h2_qrcode_decoder.h](include/h2_qrcode_decoder.h) defines dimensions, stride, capacity, output preservation and synchronous lifetime rules. Decoding uses a pinned quirc dependency with per-instance allocation callbacks backed by the caller's Memory PAL. The decoder selects no camera, OS or radio and retains no input after returning.

Run `:decoder_test` for encoder/decoder round trips across error-correction levels, rotation and mirroring, plus stride, capacity, invalid dimensions and allocation failures. Run `:qrcode_test` for the existing encoder/rasterizer contract.
