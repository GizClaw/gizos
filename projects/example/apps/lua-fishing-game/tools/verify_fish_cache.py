#!/usr/bin/env python3
"""Compare retained fish geometry and bag sprites to the actual uncached renderer."""
import argparse
import os
from pathlib import Path
import subprocess
from PIL import Image

p = argparse.ArgumentParser()
p.add_argument('--binary', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--sequence-only', action='store_true', help='Compare 110 consecutive settlement frames')
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=True)
cases = [('deck-demo', t, 0) for t in (0, 50, 349, 360, 500, 725, 949, 970, 1150, 1325, 1500, 1799, 1800)]
cases += [('fish-bag-preview', 1000, s) for s in (0, 1, 37, 95, 192, 1000)]
if a.sequence_only:
    cases = []
for scene, time, scroll in cases:
    frames = []
    for cached in (False, True):
        target = a.out / f'{scene}-{time}-{scroll}-{cached}.ppm'
        command = [str(a.binary.resolve()), f'--scene={scene}', f'--time-ms={time}',
                   f'--scroll={scroll}', f'--capture={target.resolve()}']
        if not cached:
            command += ['--no-cache']
        run = subprocess.run(command, env={**os.environ, 'SDL_VIDEODRIVER': 'dummy'},
                             capture_output=True, text=True, timeout=120)
        (target.with_suffix('.log')).write_text(run.stdout + run.stderr)
        assert run.returncode == 0, run.stdout + run.stderr
        frames.append(Image.open(target).convert('RGB'))
    first, second = frames
    xy = [(x, y) for y in range(first.height) for x in range(first.width)
          if first.getpixel((x, y)) != second.getpixel((x, y))]
    # Cached hairlines are rasterized before scroll clipping, unlike the legacy
    # endpoint-clipped Bresenham path. Permit only bounded one-pixel edge changes.
    assert len(xy) <= (128 if scene == 'fish-bag-preview' else 0), (scene, time, scroll, len(xy))
    for x, y in xy:
        neighbors = [(xx, yy) for yy in range(max(0, y-1), min(first.height, y+2))
                     for xx in range(max(0, x-1), min(first.width, x+2))]
        assert first.getpixel((x, y)) in [second.getpixel(q) for q in neighbors]
        assert second.getpixel((x, y)) in [first.getpixel(q) for q in neighbors]
    print(f'PASS {scene} time={time} scroll={scroll}: {len(xy)} edge pixels differ', flush=True)
if a.sequence_only:
    for cached in (False, True):
        prefix = a.out / f'{cached}-'
        command = [str(a.binary.resolve()), '--scene=deck-record', '--record-frames=110',
                   f'--record-prefix={prefix.resolve()}'] + ([] if cached else ['--no-cache'])
        run = subprocess.run(command, env={**os.environ, 'SDL_VIDEODRIVER': 'dummy'},
                             capture_output=True, text=True, timeout=120)
        assert run.returncode == 0, run.stdout + run.stderr
    for i in range(110):
        first = Image.open(a.out / f'False-{i:05d}.ppm')
        second = Image.open(a.out / f'True-{i:05d}.ppm')
        assert first.tobytes() == second.tobytes(), ('settlement sequence mismatch', i)
    print('PASS 110 consecutive settlement frames: pixel identical', flush=True)
