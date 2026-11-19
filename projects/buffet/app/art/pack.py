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


def emit(file, kind, name, values):
    print(f'static const {kind} {name}[] = {{', file=file)
    for i in range(0, len(values), 64):
        print(','.join(map(str, values[i:i+64])) + ',', file=file)
    print('};', file=file)


def main():
    with Path(sys.argv[1]).open('w', encoding='ascii') as out:
        print('#include "buffet_sprite.h"', file=out)
        for arg in sys.argv[2:]:
            source = Path(arg)
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
            emit(out, 'uint8_t', name + '_pixels', pixels)
            emit(out, 'uint16_t', name + '_rgb', palette)
            emit(out, 'uint8_t', name + '_alpha', alpha)
            print(f'const bf_sprite_t bf_{name} = {{{w},{h},{name}_pixels,{name}_rgb,{name}_alpha}};', file=out)

if __name__ == '__main__':
    main()
