#!/usr/bin/env python3
"""Create 16 kHz PAL WAVs and MP3 previews using FFmpeg."""

import argparse
from array import array
from pathlib import Path
import subprocess
import sys
import wave

MASTER_RATE = 24_000
PAL_RATE = 16_000
FULL = 'lanterns-on-the-belt.wav'
COMPACT = 'lanterns-on-the-belt-compact.wav'
GAIN = {
    'plate-pickup.wav': .65, 'bite.wav': .62, 'stomach-red.wav': .52,
    'card-flip.wav': .60, 'card-reveal.wav': .66, 'admission.wav': .55,
    'stomach-recover.wav': .43, 'closing.wav': .56,
}


def ffmpeg(*command, data=None):
    return subprocess.run(['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error',
                           *map(str, command)], input=data, stdout=subprocess.PIPE,
                          check=True).stdout


def read_wav(path):
    with wave.open(str(path), 'rb') as source:
        assert (source.getnchannels(), source.getsampwidth(), source.getframerate()) == (1, 2, MASTER_RATE)
        return source.readframes(source.getnframes())


def write_wav(path, payload):
    with wave.open(str(path), 'wb') as output:
        output.setparams((1, 2, PAL_RATE, 0, 'NONE', 'not compressed'))
        output.writeframes(payload)


def resample_pcm(payload):
    return ffmpeg('-f', 's16le', '-ar', MASTER_RATE, '-ac', 1, '-i', 'pipe:0',
                  '-af', f'aresample={PAL_RATE}:filter_size=64', '-f', 's16le',
                  'pipe:1', data=payload)


def pal_export(path, payload, loop=False):
    size = len(payload) * PAL_RATE // MASTER_RATE
    if loop:
        # Supply wrapped samples so the filter crosses EOF exactly as it would
        # in a steady-state loop. 960 input frames also align to the 3:2 ratio.
        pad_bytes = 2 * 960
        input_with_wrap = payload[-pad_bytes:] + payload + payload[:pad_bytes]
        output_offset = pad_bytes * PAL_RATE // MASTER_RATE
        resampled = resample_pcm(input_with_wrap)[output_offset:output_offset + size]
    else:
        samples = read_samples(resample_pcm(payload))
        # Suppress the sub-ms filter pre-ring at a one-shot's silent boundaries.
        taper = PAL_RATE // 1000
        for index in range(taper):
            phase = index / taper
            window = phase * phase * (3 - 2 * phase)
            samples[index] = round(samples[index] * window)
            samples[-1 - index] = round(samples[-1 - index] * window)
        if sys.byteorder != 'little':
            samples.byteswap()
        resampled = samples.tobytes()
    assert len(resampled) == size, (path, len(resampled), size)
    write_wav(path, resampled)


def read_samples(payload):
    samples = array('h')
    samples.frombytes(payload)
    if sys.byteorder != 'little':
        samples.byteswap()
    return samples


def cue_preview(full, cues, destination):
    # Dry cues first, a real concatenated loop seam at 11.3s, then in-game gains.
    reel = array('d', [0.0]) * round(16.8 * MASTER_RATE)
    dry_schedule = [(.20, 'plate-pickup.wav'), (.93, 'bite.wav'),
                    (1.66, 'stomach-red.wav'), (2.66, 'card-flip.wav'),
                    (3.39, 'card-reveal.wav'), (5.00, 'admission.wav'),
                    (6.00, 'stomach-recover.wav'), (6.95, 'closing.wav')]
    for when, name in dry_schedule:
        start = round(when * MASTER_RATE)
        for index, sample in enumerate(read_samples(cues[name])):
            reel[start + index] += sample * .95

    music = read_samples(full)
    start, stop, seam = (round(time * MASTER_RATE) for time in (8.50, 16.75, 11.30))
    fade = round(.25 * MASTER_RATE)
    for index in range(start, stop):
        envelope = min(1.0, (index - start) / fade, (stop - index - 1) / fade)
        reel[index] += .50 * envelope * music[(index - seam) % len(music)]
    for when, name in [(12.50, 'plate-pickup.wav'), (12.78, 'bite.wav'),
                       (13.32, 'stomach-red.wav'), (14.40, 'stomach-recover.wav'),
                       (15.13, 'card-flip.wav'), (15.32, 'card-reveal.wav')]:
        start = round(when * MASTER_RATE)
        for index, sample in enumerate(read_samples(cues[name])):
            reel[start + index] += GAIN[name] * sample

    assert max(map(abs, reel)) < 32767
    pcm = array('h', map(round, reel))
    if sys.byteorder != 'little':
        pcm.byteswap()
    ffmpeg('-f', 's16le', '-ar', MASTER_RATE, '-ac', 1, '-i', 'pipe:0',
           '-c:a', 'libmp3lame', '-b:a', '64k', destination, data=pcm.tobytes())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    parser.add_argument('compact_master', type=Path)
    args = parser.parse_args()
    pal = args.bundle / 'pal16'
    preview = args.bundle / 'preview'
    pal.mkdir(parents=True, exist_ok=True)
    preview.mkdir(parents=True, exist_ok=True)

    full_path = args.bundle / 'runtime' / FULL
    full = read_wav(full_path)
    pal_export(pal / FULL, full, loop=True)
    pal_export(pal / COMPACT, read_wav(args.compact_master), loop=True)
    cues = {name: read_wav(args.bundle / 'runtime' / name) for name in GAIN}
    for name, payload in cues.items():
        pal_export(pal / name, payload)

    ffmpeg('-i', full_path, '-c:a', 'libmp3lame', '-b:a', '64k',
           '-metadata', 'title=Lanterns on the Belt', preview / 'lanterns-on-the-belt.mp3')
    cue_preview(full, cues, preview / 'cue-and-loop-reel.mp3')


if __name__ == '__main__':
    main()
