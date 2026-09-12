#!/usr/bin/env python3
"""A/B actual RGB565 inventory captures, with bounded one-pixel raster variance.

Pre-rasterized integer translations can round sprite edges differently from
absolute float geometry. Require <=128 differing pixels and matching colors in
both one-pixel neighborhoods; never tolerate changed panels, labels or clipping.
Requires Pillow and an SDL dummy driver (no replacement renderer).
"""
import argparse
import os
from pathlib import Path
import subprocess
from PIL import Image

p = argparse.ArgumentParser()
p.add_argument('--binary', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--layout', choices=('amoled', 'h106'), default='amoled')
p.add_argument('--info-only', action='store_true', help='check only the information panel; does not certify thumbnail equivalence')
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=True)
for scene in ('rods', 'reels', 'lures'):
    for scroll in (0, 1, 37, 95, 192, 1000):
        frames = []
        for cached in (False, True):
            target = a.out / f'{scene}-{scroll}-{cached}.ppm'
            result = subprocess.run(
                [str(a.binary.resolve()), '--scene=' + scene,
                 '--scroll=' + str(scroll), '--time-ms=1000',
                 '--capture=' + str(target.resolve())] + ([] if cached else ['--no-cache']),
                env={**os.environ, 'SDL_VIDEODRIVER': 'dummy'},
                capture_output=True, text=True, timeout=120)
            (target.with_suffix('.log')).write_text(result.stdout + result.stderr)
            assert result.returncode == 0, result.stdout + result.stderr
            frames.append(Image.open(target).convert('RGB'))
        first, second = frames
        width, height = (240, 240) if a.layout == 'h106' else (368, 448)
        assert first.size == second.size == (width, height)
        left, right = first.load(), second.load()
        top = (178 if a.layout == 'h106' else 360) if a.info_only else 0
        differences = [(x, y) for y in range(top, height) for x in range(width)
                       if left[x, y] != right[x, y]]
        tolerance = 0 if a.layout == 'h106' else 128
        assert len(differences) <= tolerance, (scene, scroll, 'excess pixel changes', len(differences))
        for x, y in differences:
            neighborhood = [(xx, yy) for yy in range(max(0, y-1), min(height, y+2))
                            for xx in range(max(0, x-1), min(width, x+2))]
            assert any(left[x, y] == right[xy] for xy in neighborhood), (scene, scroll, x, y)
            assert any(right[x, y] == left[xy] for xy in neighborhood), (scene, scroll, x, y)
        print(f'PASS {scene} scroll={scroll} scope={"info" if a.info_only else "full"}: {len(differences)} edge pixels differ')
