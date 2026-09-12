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
        assert first.size == second.size == (368, 448)
        left, right = first.load(), second.load()
        differences = [(x, y) for y in range(448) for x in range(368)
                       if left[x, y] != right[x, y]]
        assert len(differences) <= 128, (scene, scroll, 'excess pixel changes', len(differences))
        for x, y in differences:
            neighborhood = [(xx, yy) for yy in range(max(0, y-1), min(448, y+2))
                            for xx in range(max(0, x-1), min(368, x+2))]
            assert any(left[x, y] == right[xy] for xy in neighborhood), (scene, scroll, x, y)
            assert any(right[x, y] == left[xy] for xy in neighborhood), (scene, scroll, x, y)
        print(f'PASS {scene} scroll={scroll}: {len(differences)} edge pixels differ')
