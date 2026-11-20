"""Export the dishes and inked HUD pieces used by the geometry-led room."""
from array import array
import struct
import sys
import zlib
from PIL import Image

NAMES = ('salmon', 'tamago', 'tuna', 'shrimp', 'maki', 'inari', 'ramen', 'custard')
DITHER = (0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5)


def rgb565(image):
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
    stations = source/'stations'
    for name in ('next','fullness','clock-bg','clock','plates','arrival'):
        texture(Image.open(stations/'hud'/f'{name}.png'),out,'hud_'+name.replace('-','_'))
    menu = stations/'menu'
    texture(Image.open(menu/'plate-shadow.png'),out,'plate_shade',(128,102))
    for name in NAMES:
        plate = Image.open(menu/'ceramics'/f'{name}.png').convert('RGBA')
        ingredient = Image.open(menu/'ingredients'/f'{name}.png').convert('RGBA')
        plated = plate.copy()
        plated.alpha_composite(Image.open(menu/'contact'/f'{name}.png').convert('RGBA'))
        plated.alpha_composite(ingredient)
        texture(plated,out,name)
        texture(plated,out,name+'_table',(128,102))
        texture(Image.open(menu/'held'/f'{name}.png'),out,name+'_bite',(60,44))
        if name=='salmon':
            texture(plate,out,'empty')
