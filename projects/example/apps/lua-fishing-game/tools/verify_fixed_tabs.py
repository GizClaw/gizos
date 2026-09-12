#!/usr/bin/env python3
"""Verify actual rendered wood/frame pixels remain fixed through tab movement."""
import argparse
import os
from pathlib import Path
import subprocess
from PIL import Image, ImageDraw

p = argparse.ArgumentParser()
p.add_argument('--binary', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--layout', choices=('amoled', 'h106'), default='amoled')
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=True)
width = 448 if a.layout == 'h106' else 368
images = []
for source, target in ((1, 2), (2, 3), (3, 4), (4, 3)):
    baseline = None
    for offset in (0, width // 4, width // 2, width * 3 // 4, width):
        path = a.out / f'{source}-{target}-{offset}.ppm'
        run = subprocess.run([str(a.binary.resolve()), '--scene=tab-preview',
                              f'--rod={source}', f'--reel={target}', f'--scroll={offset}',
                              '--time-ms=1000', '--capture=' + str(path.resolve())],
                             env={**os.environ, 'SDL_VIDEODRIVER': 'dummy'},
                             capture_output=True, timeout=60)
        assert run.returncode == 0, run.stderr.decode()
        frame = Image.open(path).convert('RGB')
        assert frame.size == ((240, 240) if a.layout == 'h106' else (368, 448))
        assert (248, 0, 248) not in frame.getdata(), 'transparent key leaked onto the display'
        # Sampling rows strictly inside the fixed wood gutters, away from cards/header.
        rows = (4, 56, 327, 447) if a.layout == 'h106' else (4, 56, 355, 447)
        rows = tuple(int(y * 240 / 448) for y in rows) if a.layout == 'h106' else rows
        fixed = [frame.crop((0, y, frame.width, y + 1)).tobytes() for y in rows]
        if baseline is None:
            baseline = fixed
        assert fixed == baseline, (source, target, offset, 'wood moved')
        frame.save(path.with_suffix('.png'))
        if source == 1:
            images.append(frame)
    print(f'PASS {source}->{target}: fixed wood/frame gutters; no key-color leaks', flush=True)
sheet = Image.new('RGB', (images[0].width * len(images), images[0].height + 22), '#20242b')
draw = ImageDraw.Draw(sheet)
for i, frame in enumerate(images):
    x = i * frame.width
    draw.text((x + 6, 5), f'{i * 25}%', fill='white')
    sheet.paste(frame, (x, 22))
sheet.save(a.out / 'fixed-tabs.png')
