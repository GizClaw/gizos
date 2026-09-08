#!/usr/bin/env python3
"""Compare real SDL PAL captures with the approved Canvas reference frames.

Usage: python3 app/tools/compare_layers.py --captures /tmp --layer walls --prefix qi-duel-native-walls-
Run wall_reference.py /bake first. Captures come from the native target's
--layer=walls --time-ms=N --capture=PATH, never from a second mock renderer.
"""
import argparse
import json
from pathlib import Path
import struct
import zlib

APP = Path(__file__).resolve().parents[2]
QA = APP / 'desktop-preview/comparison'
TIMES = (0, 1875, 4000, 9000, 17000)
W, H = 368, 448


def read_ppm(path):
    data = path.read_bytes()
    header = b'P6\n368 448\n255\n'
    assert data.startswith(header) and len(data) == len(header) + W * H * 3
    return data[len(header):]


def write_png(path, rgb, width=W, height=H):
    def chunk(kind, payload):
        return struct.pack('>I', len(payload)) + kind + payload + struct.pack(
            '>I', zlib.crc32(kind + payload))
    scanlines = b''.join(b'\0' + rgb[y*width*3:(y+1)*width*3] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' +
        chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
        chunk(b'IDAT', zlib.compress(scanlines, 9)) + chunk(b'IEND', b''))


def quantize(rgb):
    return bytes(((v >> 2) << 2 | (v >> 2) >> 4) if i % 3 == 1
                 else ((v >> 3) << 3 | (v >> 3) >> 2) for i, v in enumerate(rgb))


def metrics(reference, native):
    differences = [abs(a-b) for a, b in zip(reference, native)]
    active = [differences[i] for i in range(len(native)) if reference[i] or native[i]]
    return dict(mean_absolute_error=sum(differences)/len(differences),
                lit_channel_mean_error=sum(active)/max(1, len(active)),
                max_channel_error=max(differences),
                identical_channel_percent=100*sum(d == 0 for d in differences)/len(differences))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--captures', type=Path, required=True)
    parser.add_argument('--prefix', default='qi-duel-native-walls-')
    parser.add_argument('--layer', choices=['walls', 'wheel', 'arena', 'dust', 'particles',
        'opponent', 'hand-left', 'hand-right', 'arena-dust', 'scene7', 'hud', 'scene8',
        'player-down', 'player-up', 'enemy-down', 'enemy-up','carousel-frame',
        'charge-cells','charge-base','charge-up','charge-down','scene11','carousel',
        'skill-charge','skill-wave','skill-absorb','skill-guard','carousel-wave',
        'carousel-absorb','carousel-guard','carousel-left','carousel-right','full'], default='walls')
    parser.add_argument('--times', default=','.join(map(str,TIMES)))
    args = parser.parse_args()
    reports = []
    for time in map(int,args.times.split(',')):
        browser = read_ppm(QA / f'browser-{args.layer}-{time}.ppm')
        native = read_ppm(args.captures / f'{args.prefix}{time}.ppm')
        report = dict(time_ms=time, original_rgb888=metrics(browser, native),
                      equal_rgb565=metrics(quantize(browser), native))
        reports.append(report)
        write_png(QA / f'browser-{args.layer}-{time}.png', browser)
        write_png(QA / f'sdl-{args.layer}-{time}.png', native)
        paired = b''.join(browser[y*W*3:(y+1)*W*3] + native[y*W*3:(y+1)*W*3]
                          for y in range(H))
        write_png(QA / f'pair-{args.layer}-{time}.png', paired, width=W*2)
        print(json.dumps(report))
    (QA / f'{args.layer}-report.json').write_text(json.dumps(reports, indent=2) + '\n')


if __name__ == '__main__':
    main()
