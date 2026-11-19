"""Prepare the restaurant's independent painted layers and dish mipmaps."""
from array import array
from pathlib import Path
import struct
import sys
import zlib

from PIL import Image, ImageOps

NAMES = ('salmon', 'tamago', 'tuna', 'shrimp', 'maki', 'inari')
DITHER = (0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5)


def rgb565(image):
    # Dither only at the final pixel density, with the same threshold for
    # each colour channel to keep the sunlit timber and glaze neutral.
    image = image.convert('RGB')
    words = array('H')
    for i, (r, g, b) in enumerate(image.getdata()):
        y, x = divmod(i, image.width)
        q = 16 * DITHER[(y % 4) * 4 + x % 4]
        words.append((min(31, (r * 31 + q) // 255) << 11) |
                     (min(63, (g * 63 + q) // 255) << 5) |
                     min(31, (b * 31 + q) // 255))
    if sys.byteorder != 'little':
        words.byteswap()
    return words.tobytes()


def texture(image, out, name, size=None):
    image = image.convert('RGBA')
    if size:
        image = image.resize(size, Image.Resampling.LANCZOS)
    header = struct.pack('<HH', image.width, image.height)
    payload = header + rgb565(image) + image.getchannel('A').tobytes()
    (out / f'{name}.paintz').write_bytes(zlib.compress(payload, 9))


def service_layers(source, out):
    room = source / 'room'
    floor = Image.open(room/'architecture.png')
    if floor.size != (1600, 960):
        raise ValueError('the room must match the native framebuffer')
    (out/'room.rgb565z').write_bytes(zlib.compress(rgb565(floor), 9))
    for name, path, size in (
        ('counter', 'island.png', None), ('counter_shade', 'island-shadow.png', None),
        ('steel', 'belt-slat.png', None), ('mat', 'props/mat.png', (404, 124)),
        ('yuzu', 'props/yuzu.png', (134, 162)), ('tea', 'props/tea.png', (86, 48)),
        ('condiments', 'props/condiments.png', (84, 62)),
        ('booth', 'props/booth.png', (260, 226)),
    ):
        texture(Image.open(room/path), out, name, size)

    menu = source/'menu'
    texture(Image.open(menu/'ground'/'plate-shadow.png'), out, 'plate_shade', (160, 104))
    for name in NAMES:
        plate = Image.open(menu/'ceramics'/f'{name}.png').convert('RGBA')
        ingredient = Image.open(menu/'ingredients'/f'{name}.png').convert('RGBA')
        plated = plate.copy()
        plated.alpha_composite(Image.open(menu/'food-contact'/f'{name}.png').convert('RGBA'))
        plated.alpha_composite(ingredient)
        texture(plated, out, name)
        texture(plated, out, name+'_belt', (160, 104))
        texture(plate, out, name+'_cleared', (160, 104))
        # A food-only cutout travels from the serving to the character's hand.
        texture(ingredient.crop(ingredient.getbbox()), out, name+'_bite', (80, 48))
        if name == 'salmon':
            texture(plate, out, 'empty')
