"""Embed baked 8-bit indexed PNGs as ROM pixels and RGB565 palettes.

The authoring packages are optional. The normal Bazel build uses the Python
standard library and the runtime can blit a sheet without heap decoding.
"""
import struct
import sys
from pathlib import Path
import zlib


def decode(source):
    data = source.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'invalid PNG: {source}')
    offset = 8
    chunks = {}
    while offset < len(data):
        size, kind = struct.unpack_from('>I4s', data, offset)
        end = offset + 8 + size
        payload = data[offset + 8:end]
        if zlib.crc32(kind + payload) != struct.unpack_from('>I', data, end)[0]:
            raise ValueError(f'bad checksum: {source}')
        chunks[kind] = chunks.get(kind, b'') + payload
        offset = end + 4
    w, h, bits, kind, *options = struct.unpack('>IIBBBBB', chunks[b'IHDR'])
    if not (0 < w <= 4096 and 0 < h <= 4096 and bits == 8 and kind == 3 and options == [0, 0, 0]):
        raise ValueError(f'expected noninterlaced indexed plate: {source}')
    raw = zlib.decompress(chunks[b'IDAT'])
    if len(raw) != (w + 1) * h:
        raise ValueError(f'wrong scanline length: {source}')
    pixels = bytearray()
    for off in range(0, len(raw), w + 1):
        # Pillow writes unfiltered palette scanlines. Refuse other encodings
        # explicitly instead of embedding filtered bytes as colour indices.
        if raw[off] != 0:
            raise ValueError(f'rebake filtered plate: {source}')
        pixels += raw[off+1:off+1+w]
    rgb = chunks[b'PLTE']
    palette = [(r >> 3) << 11 | (g >> 2) << 5 | b >> 3 for r, g, b in zip(rgb[::3], rgb[1::3], rgb[2::3])]
    alpha = chunks.get(b'tRNS', b'').ljust(len(palette), b'\xff')
    return w, h, pixels, palette, alpha


def emit(file, kind, name, values, public=False):
    storage = '' if public else 'static '
    print(f'{storage}const {kind} {name}[] = {{', file=file)
    for i in range(0, len(values), 64):
        print(','.join(map(str, values[i:i+64])) + ',', file=file)
    print('};', file=file)


def pack_track(source, out):
    raw = zlib.decompress(source.read_bytes())
    magic, width, height, count = struct.unpack_from('<4sHHI', raw)
    if (magic, width, height) != (b'TWU1', 1056, 280):
        raise ValueError('unexpected conveyor registration')
    data = list(v[0] for v in struct.iter_unpack('<H', raw[12:]))
    start = 0
    for _ in range(count):
        y, x, length = data[start:start+3]
        if y >= height or not 0 < length <= width - x:
            raise ValueError('conveyor run crosses the island bounds')
        start += 3 + length
    if start != len(data):
        raise ValueError('invalid conveyor run length')
    emit(out, 'uint16_t', 'bf_belt_runs', data, public=True)
    print(f'const unsigned bf_belt_run_count = {count};', file=out)


def compact_plane(values, width, height, output_width, output_height):
    """Resample a prefiltered sprite at board display scale."""
    return [values[min(height - 1, (2*y+1)*height//(2*output_height))*width +
                   min(width - 1, (2*x+1)*width//(2*output_width))]
            for y in range(output_height) for x in range(output_width)]


def pack_paint(source, out, compact):
    name = source.stem
    raw = zlib.decompress(source.read_bytes())
    w, h = struct.unpack_from('<HH', raw)
    size = w * h
    if not (0 < w <= 1600 and 0 < h <= 960 and len(raw) == 4 + 3*size):
        raise ValueError(f'invalid painted layer: {name}')
    rgb = [v[0] for v in struct.iter_unpack('<H', raw[4:4+2*size])]
    alpha = raw[4 + 2*size:]
    if compact:
        max_width = 112 if name in ('salmon', 'tamago', 'tuna', 'shrimp', 'maki', 'inari', 'ramen', 'custard', 'empty') else 48
        if name.startswith('hud_'):
            max_width = min(106, w)
        nw = min(max_width, w)
        nh = max(1, round(h*nw/w))
        rgb = compact_plane(rgb, w, h, nw, nh)
        alpha = compact_plane(alpha, w, h, nw, nh)
        w, h = nw, nh
    emit(out, 'uint16_t', name + '_rgb', rgb)
    emit(out, 'uint8_t', name + '_alpha', alpha)
    print(f'const bf_paint_t bf_{name} = {{{w},{h},{name}_rgb,{name}_alpha}};', file=out)


def main():
    compact = '--board' in sys.argv[2:]
    with Path(sys.argv[1]).open('w', encoding='ascii') as out:
        print('#include "buffet_sprite.h"', file=out)
        for arg in sys.argv[2:]:
            if arg == '--board':
                continue
            source = Path(arg)
            if source.suffix == '.paintz':
                pack_paint(source, out, compact)
                continue
            if source.name == 'belt-uv-runs.z':
                pack_track(source, out)
                continue
            if source.suffix == '.rgb565z':
                raw = zlib.decompress(source.read_bytes())
                if len(raw) != 1600*960*2: raise ValueError('wrong painted room extent')
                print('const uint16_t bf_floor_pixels[] = {', file=out)
                for off in range(0,len(raw),128):
                    print(','.join(map(str, struct.unpack_from('<64H',raw,off)))+',',file=out)
                print('};',file=out)
                continue
            name = source.stem
            w, h, pixels, palette, alpha = decode(source)
            if compact and name not in ('gauge', 'gauge_full', 'mark'):
                factor = 4 if name in ('cloth','cast') else 2
                nw, nh = max(1, w//factor), max(1, h//factor)
                pixels = compact_plane(pixels, w, h, nw, nh)
                w, h = nw, nh
            emit(out, 'uint8_t', name + '_pixels', pixels)
            emit(out, 'uint16_t', name + '_rgb', palette)
            emit(out, 'uint8_t', name + '_alpha', alpha)
            print(f'const bf_sprite_t bf_{name} = {{{w},{h},{name}_pixels,{name}_rgb,{name}_alpha}};', file=out)

if __name__ == '__main__':
    main()
