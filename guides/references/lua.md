# Lua Display API

<!--@include: ../.generated/api/lua.md-->

Built-in numeric buffers, physics and geometry: [Lua numeric API](./lua-numeric.md).

通用 C 像素核心见 [Raster2D API](./raster2d.md)。Lua Display 的 VM 数据、损伤与生命周期由本模块拥有。

## Regions from strings

`display.region_from_string(width, height, data[, encoding])` creates an opaque region for `display.draw_region(region, x, y, ...)` and, for a matching full-screen image, `display.restore_background(region)`. It does not acquire or draw to the display. Width and height must be integers from 1 through 4096; `data` must be a Lua string. The default encoding is `"rgb565be"`.

- `"rgb565be"`: exactly `width * height * 2` binary bytes, row-major, high byte first for each RGB565 pixel (red = `F8 00`, green = `07 E0`, blue = `00 1F`).
- `"rgb565be-lz4-b85"`: eight ASCII hexadecimal digits containing the compressed byte length, followed by Python `base64.b85encode(block, pad=True)` text. The block is a standard raw LZ4 block, without a frame or stored output size. The decoded size must equal `width * height * 2`. All Base85 groups and zero padding, lengths, LZ4 sequences, offsets and output bounds are checked. No whitespace or trailing data is accepted. LZ4 final literals and last-match distance must obey the block format's end conditions.

```lua
local display = require('display')
local red = display.region_from_string(1, 1, string.char(0xf8, 0))
display.draw_region(red, 0, 0)
display.present()
```

Decoding writes directly into native region storage, without a pixel table or full intermediate decompression buffer. The region is Lua userdata: pixels, row metadata and damage tiles count toward the VM memory quota and are reclaimed by garbage collection. The input string may be released after construction; its own storage also counts toward the quota while live. Invalid input raises a Lua error; quota exhaustion raises the normal Lua memory error. Failed decoding leaves no published region, and its temporary userdata is garbage collectable. A region installed as a background remains retained until background release.
