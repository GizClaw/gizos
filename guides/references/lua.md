# Lua Display API

<!--@include: ../.generated/api/lua.md-->

Built-in numeric buffers, physics and geometry: [Lua numeric API](./lua-numeric.md).

通用 C 像素核心见 [Raster2D API](./raster2d.md)。Lua Display 的 VM 数据、损伤与生命周期由本模块拥有。

## Quad strip batches

Compile the normalized layout once, then supply the current four corners and colors. Records draw in array order; a five-field record applies a transverse patch before the strip interpolation. See the generated contract above for limits, arithmetic and error behavior.

```lua
local display = require('display')
local bands = display.compile_quad_batch({
    {0, 0.5, 1},             -- direct strip, first half
    {0.5, 1, 2},             -- direct strip, second half
    {0, 0.1, 3, 0.25, 0.75} -- strip inside a transverse patch
})
local colors = display.compile_palette({'blue', 'red', 'white'})
display.draw_quad_batch(bands, colors, 10,10, 200,30, 180,210, 40,190)
display.present()
```

For dirty-row repair, append `clip_top, clip_bottom` to the draw call, for example `display.draw_quad_batch(bands, colors, 10,10, 200,30, 180,210, 40,190, 16,32)` to touch only rows 16 through 31. Missing or `nil` bounds default to 0 and the display height. These integer framebuffer bounds only restrict raster rows; they do not alter the corners or the normalized patch intervals. Disjoint clips can reproduce a full draw when each row retains the same painter order. Empty clips still validate all inputs and require a live display.

For changing RGB888 colors, pass a reused array of ordinary Display color tables instead of a compiled palette. Each slot is sampled during the call; retain distinct tables for colors that differ. This API only expands caller-specified geometry and reuses the existing polygon raster. It does not decide lighting, projection or scene order.

## Precomposed quad materials

For repeatedly drawing overlapping strips on a moving quad, compile the batch into a material once. The material resolves the last covering record's palette index in normalized coordinates, then maps that final field in one scan. Geometry, colors, cache keys and cache lifetime remain in Lua.

```lua
local material = display.compile_quad_material(bands)
local used_material = display.draw_quad_material(material, colors,
    10,10, 200,30, 180,210, 40,190, 16,32)
```

This is an explicit raster choice: half-open parameter cells and Q24 grid boundaries can differ at edge pixels from the existing polygon batch. Non-convex, degenerate or unsafe numeric quads replay the original batch and return `false`; successful material draws return `true`. Disjoint row clips reproduce a full material draw. See the generated contract for precise bounds, rounding, memory and error semantics. Existing palettes and `capture_region` / `draw_region` provide color reuse and optional pixel caching without another cache API. Both Web and embedded builds use this portable implementation.

### Source cropping and projective depth

A fixed source profile can move through projected slices without recompiling its boundaries. Pass the source U interval covered by the slice and its endpoint depth ratio to the projective draw. The quad corners must describe that cropped interval's projected endpoints, with A/D at the first endpoint and B/C at the last. Lua computes those corners, source units, depth values and colors.

```lua
-- Source U .2 through .8; last endpoint depth is twice the first.
display.draw_quad_material_projective(material, colors,
    10,10, 200,30, 180,210, 40,190, .2,.8,2, 16,32)
```

For local source fraction `s`, the target fraction is `s*r/(1+(r-1)*s)`, where `r=Z_last/Z_first`. This matches reciprocal-depth projection of a linearly parameterized source interval. V remains bilinear, and the existing half-open/Q24 material pixel rules still apply. Whole-source mapping with ratio 1 reproduces the original material draw. Empty source crops draw nothing after validation; unsupported geometry replays clipped, projected polygon records. Each draw reuses the compiled material and fixed scan scratch without allocating. See the generated contract for limits and fallback semantics.

## Regions from strings

`display.region_from_string(width, height, data[, encoding])` creates an opaque region for `display.draw_region(region, x, y, ...)` and, for a matching full-screen image, `display.restore_background(region)`. The constructor does not acquire or draw to the display and remains usable on an existing proxy after `deinit`. Initial `require('display')` still acquires Display and raises on failure; cached `require` does not reopen it after `deinit`, and drawing still requires a live acquisition. Width and height must be integers from 1 through 4096; `data` must be a Lua string. The default encoding is `"rgb565be"`.

- `"rgb565be"`: exactly `width * height * 2` binary bytes, row-major, high byte first for each RGB565 pixel (red = `F8 00`, green = `07 E0`, blue = `00 1F`).
- `"rgb565be-lz4-b85"`: eight ASCII hexadecimal digits containing the compressed byte length, followed by Python `base64.b85encode(block, pad=True)` text. The block is a standard raw LZ4 block, without a frame or stored output size. The decoded size must equal `width * height * 2`. All Base85 groups and zero padding, lengths, LZ4 sequences, offsets and output bounds are checked. No whitespace or trailing data is accepted. LZ4 final literals and last-match distance must obey the block format's end conditions.

```lua
local display = require('display')
local red = display.region_from_string(1, 1, string.char(0xf8, 0))
display.draw_region(red, 0, 0)
display.present()
```

Decoding writes directly into native region storage, without a pixel table or full intermediate decompression buffer. The region is Lua userdata: pixels, row metadata and damage tiles count toward the VM memory quota and are reclaimed by garbage collection. The input string may be released after construction; its own storage also counts toward the quota while live. Invalid input raises a Lua error; quota exhaustion raises the normal Lua memory error. Failed decoding leaves no published region, and its temporary userdata is garbage collectable. A region installed as a background remains retained until background release.
