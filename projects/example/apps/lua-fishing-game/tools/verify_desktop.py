#!/usr/bin/env python3
"""Capture the actual native Lua->RGB565->SDL output, never a replica renderer.
Requires Pillow. Pass an output directory that does not already contain captures.
"""
import argparse
from pathlib import Path
import subprocess
from PIL import Image, ImageDraw

parser = argparse.ArgumentParser()
parser.add_argument('--binary', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
a = parser.parse_args()
a.out.mkdir(parents=True, exist_ok=True)
scenes = ['idle', 'overhead', 'pendulum', 'iso', 'fly-back', 'fly-send', 'fight', 'rods', 'reels', 'lures']
images = {}
cases = [(scene, scene, []) for scene in scenes] + [
    ('cq', 'cq', []),
    ('cq-menu', 'reels', ['--reel=10']),
    ('cq-mounted', 'idle', ['--reel=10']),
    ('fly-gear', 'reels', ['--rod=7', '--reel=6']),
]
for scene, mode, extra in cases:
    ppm = a.out / (scene + '.ppm')
    command = [str(a.binary.resolve()), '--scene=' + mode, '--time-ms=1000', '--capture=' + str(ppm.resolve()), '--check'] + extra
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    (a.out / (scene + '.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f'{scene}: native renderer failed\n{result.stdout}\n{result.stderr}')
    assert 'FISHING_CHECK PASS' in result.stdout + result.stderr, scene
    assert 'FISHING_INPUT_CHECK PASS' in result.stdout + result.stderr, scene
    im = Image.open(ppm).convert('RGB')
    assert im.size == (368, 448), (scene, im.size)
    if mode not in ['rods', 'reels', 'lures', 'cq']:
        # Anchor must physically meet the screen corner, not float above it.
        assert max(im.getpixel((367, 447))) < 110, (scene, 'rod butt missing')
        r, g, b = im.getpixel((1, 180))
        assert b > 220 and g > 130 and r < 70, (scene, 'sky')
    im.save(a.out / (scene + '.png'))
    images[scene] = im

def sheet(names, columns, path):
    gap, head, caption = 20, 34, 25
    rows = (len(names) + columns - 1) // columns
    canvas = Image.new('RGB', (columns * (368 + gap) + gap, rows * (448 + caption + gap) + head), '#0c2034')
    draw = ImageDraw.Draw(canvas)
    draw.text((gap, 12), 'NATIVE AMOLED / LUA GEOMETRY / 368 x 448', fill='#c6e7e9')
    for i, name in enumerate(names):
        x, y = gap + i % columns * (368 + gap), head + i // columns * (448 + caption + gap)
        canvas.paste(images[name], (x, y))
        draw.text((x, y + 456), name.upper(), fill='#c6e7e9')
    canvas.save(a.out / path)

sheet(['overhead', 'pendulum', 'iso', 'fly-back', 'fly-send', 'fight'], 3, 'native-storyboard.png')
sheet(['rods', 'reels', 'lures'], 3, 'native-inventory.png')
print('PASS: 14 native frames, model and gesture checks, dimensions, rod anchors and sky. Inspect PNG contact sheets visually.')
