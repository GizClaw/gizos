"""Generate the packed RGB565 zombie frames from their original drawing paths."""
import argparse
import struct
from io import BytesIO
from pathlib import Path

import cairosvg
from PIL import Image

from art import ARMOR_WALK, CHEW, RUNNER_WALK, WALK, gardener, group, svg

FRAME_SIZE = (64, 72)
ANCHOR = (32, 64)
CHEW_FAR = ((90, 152), (101, 168), -5)
CHEW_NEAR = ((69, 153), (67, 171), 0)


def rasterize(source, size, view=(0, 0, 132, 192), fit=None):
    """Apply shared world-to-pixel offset and supersample before integer-grid export."""
    w, h = size
    if fit:
        tx, ty, scale = fit
        art = source.split('>', 1)[1].rsplit('</svg>', 1)[0]
        source = svg([group([art], f'translate({tx} {ty}) scale({scale})')])
    view_text = ' '.join(str(n) for n in view)
    source = source.replace('viewBox="0 0 132 192"', f'viewBox="{view_text}"')
    source = source.replace(' width="132" height="192"', f' width="{w}" height="{h}"')
    supersampled = cairosvg.svg2png(bytestring=source.encode(), output_width=w*4, output_height=h*4)
    raster = Image.open(BytesIO(supersampled)).convert('RGBA').resize(size, Image.Resampling.LANCZOS)
    # Remove Lanczos ringing below 3% opacity and make fully transparent RGB black.
    clean = bytes(channel for px in raster.getdata() for channel in (px if px[3] >= 8 else (0,0,0,0)))
    return Image.frombytes('RGBA', size, clean)


def sprite(source):
    w, h = FRAME_SIZE
    ax, ay = ANCHOR
    scale = .311
    return rasterize(source, (w, h), (0, 0, w, h),
                     (ax - 78 * scale, ay - 183 * scale, scale))


VARIANTS = {
    'gardener': (WALK, (135,110,105,135,235), CHEW_FAR, CHEW_NEAR),
    'armored': (ARMOR_WALK, (185,140,120,180,255), ((96,156),(105,169),-4), ((61,155),(59,171),0)),
    'runner': (RUNNER_WALK, (100,80,85,95,200), ((88,149),(105,165),-12), ((63,151),(57,171),0)),
}


def frames(variant):
    walk, chew_duration, chew_far, chew_near = VARIANTS[variant]
    for name, duration, shift, head, arm, far, near in walk:
        yield name, duration, gardener(shift,head,arm,far,near,variant=variant)
    for duration, (name, _, shift, head, arm, bite) in zip(chew_duration,CHEW):
        if variant == 'runner':
            head = (head[0]-1,head[1]+1,head[2]-5)
        yield name, duration, gardener(shift,head,arm,chew_far,chew_near,bite,snooze=name=='chew_3_bite',
                                        variant=variant)
    yield 'hit_0_recoil', 115, gardener((1,3),(4,0,11),'back',chew_far,chew_near,snooze=True,variant=variant)
    yield 'defeat_0_stagger', 115, gardener((1,4),(3,5,12),'hang',chew_far,chew_near,snooze=True,variant=variant)
    yield 'defeat_1_fold', 160, gardener((2,13),(-1,9,-2),'hang',((83,158),(100,169),-4),
                                        ((69,167),(70,171),0),snooze=True,pose='defeat',variant=variant)


def rgb565_pixel(rgb):
    r,g,b = rgb
    return ((r*31+127)//255)<<11 | ((g*63+127)//255)<<5 | ((b*31+127)//255)


def decode565(color):
    return ((color>>11)*255//31, ((color>>5)&63)*255//63, (color&31)*255//31)


def pack565(images):
    """Build a shared 64-color palette, packed 2-bit alpha / 6-bit index, scanline spans."""
    px = [pixel[:3] for im in images for pixel in im.getdata() if pixel[3] >= 43]
    strip = Image.new('RGB', (len(px),1))
    strip.putdata(px)
    quantized = strip.quantize(colors=64, method=Image.Quantize.MEDIANCUT,
                               dither=Image.Dither.NONE)
    colors = quantized.getpalette()[:64*3]
    palette = list(dict.fromkeys(rgb565_pixel(tuple(colors[i:i+3])) for i in range(0,len(colors),3)))
    packed_palette = struct.pack('<'+'H'*len(palette), *palette)
    actual = [decode565(color) for color in palette]
    indices = {}
    previews, streams = [], []
    for im in images:
        values = []
        colors_out = []
        for r,g,b,a in im.getdata():
            alpha = min(3,(a+42)//85)
            if not alpha:
                values.append(0)
                colors_out.append((0,0,0,0))
                continue
            rgb = (r,g,b)
            if rgb not in indices:
                indices[rgb] = min(range(len(actual)), key=lambda i: 3*(actual[i][0]-r)**2 +
                                   5*(actual[i][1]-g)**2 + 2*(actual[i][2]-b)**2)
            index = indices[rgb]
            values.append((alpha<<6)|index)
            colors_out.append((*actual[index],alpha*85))
        packed = bytearray()
        for y in range(im.height):
            row = values[y*im.width:(y+1)*im.width]
            samples = [x for x,v in enumerate(row) if v]
            if not samples:
                packed.extend((0,0))
            else:
                start,end = samples[0], samples[-1]+1
                packed.extend((start,end-start))
                packed.extend(row[start:end])
        streams.append(bytes(packed))
        preview = Image.new('RGBA', im.size)
        preview.putdata(colors_out)
        previews.append(preview)
    return packed_palette, previews, streams


def unroll565(packed, palette, size):
    w,h = size
    colors = [decode565(n) for (n,) in struct.iter_unpack('<H', palette)]
    output = Image.new('RGBA', size)
    offset = 0
    for y in range(h):
        start,length = packed[offset:offset+2]
        offset += 2
        assert start+length <= w
        for x,v in enumerate(packed[offset:offset+length], start=start):
            if v:
                output.putpixel((x,y),(*colors[v&63],(v>>6)*85))
        offset += length
    assert offset == len(packed)
    return output


def c_array(stream, name, c_type, values, pack_width, columns):
    print(f"static const {c_type} {name}[] = {{", file=stream)
    for index in range(0, len(values), columns):
        line = ", ".join(f"0x{value:0{pack_width}x}u" for value in values[index:index + columns])
        print(f"    {line},", file=stream)
    print("};\n", file=stream)


def frames_for_export(variant):
    cels = list(frames(variant))
    for name, duration, source in cels:
        yield name, duration, sprite(source)
    yield 'portrait', 0, rasterize(cels[0][2], (64, 64), (1, 0, 119, 119))


def painted_bounds(data, width, height, palette_entries):
    rows = []
    offset = 0
    for _ in range(height):
        first, count = data[offset:offset + 2]
        offset += 2
        values = data[offset:offset + count]
        if first + count > width or any(value and not (value >> 6) for value in values):
            raise ValueError('Invalid row')
        if any((value & 63) >= palette_entries for value in values if value):
            raise ValueError('Invalid palette index')
        rows.append((first, first + count))
        offset += count
    if offset != len(data):
        raise ValueError('Invalid frame length')
    painted = [row for row, (left, right) in enumerate(rows) if left < right]
    if not painted:
        raise ValueError('Frame has no visible pixels')
    return min(rows[y][0] for y in painted), painted[0], max(rows[y][1] for y in painted), painted[-1] + 1


def export_c(output):
    with output.open('w', encoding='ascii', newline='\n') as stream:
        print('/* Generated by tools/zombie_art/generate.py. See tools/zombie_art/README.md. */\n', file=stream)
        for variant in VARIANTS:
            entries = list(frames_for_export(variant))
            palette, converted, packed = pack565([image for _, _, image in entries])
            colors = [value for (value,) in struct.iter_unpack('<H', palette)]
            c_array(stream, f'k_{variant}_palette', 'uint16_t', colors, 4, 8)
            c_array(stream, f'k_{variant}_pixels', 'uint8_t', b''.join(packed), 2, 12)
            print(f'static const zombie_frame_t k_{variant}_frames[] = {{', file=stream)
            offset = 0
            for (name, duration, image), preview, frame in zip(entries, converted, packed):
                if unroll565(frame, palette, image.size).tobytes() != preview.tobytes():
                    raise ValueError(f'{variant} {name} has an inconsistent round trip')
                left, top, right, bottom = painted_bounds(frame, image.width, image.height, len(colors))
                print(f'    {{{offset}u, {duration}u, {left}u, {top}u, {right}u, {bottom}u}}, /* {name} */', file=stream)
                offset += len(frame)
            print('};', file=stream)
            if variant != next(reversed(VARIANTS)):
                print(file=stream)
            print(f'{variant}: {len(entries)} cels, {offset + len(palette)} packed bytes')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, help='generated C include path')
    export_c(parser.parse_args().output)
