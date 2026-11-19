#!/usr/bin/env python3
"""Rebuild Fruit Slice's original sound bank using only Python's standard library.

No recordings, sound fonts, TTS voices or third-party melodies are used. The
vocal cheer is a deliberately robotic, nonverbal /wu:/ formant synthesis.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import wave

RATE = 8000
ROOT = Path(__file__).resolve().parent
NAMES = ("swing", "slice", "combo", "bomb", "miss", "fail", "result", "coin",
         "task", "purchase", "equip", "card", "rare", "cheer", "slice_jade",
         "slice_flame", "slice_bamboo", "slice_frost", "slice_storm", "music")


def envelope(t, duration, attack=0.004):
    return min(1.0, t / attack) * max(0.0, 1.0 - t / duration) ** 1.7


def tone(frequency, duration, level=0.5, glide=0.0):
    return [level * envelope(i / RATE, duration) *
            (math.sin(2 * math.pi * (frequency * i / RATE +
                                     glide * (i / RATE) ** 2 / 2)) +
             0.18 * math.sin(2 * math.pi * frequency * 2 * i / RATE)) / 1.18
            for i in range(round(duration * RATE))]


def noise(duration, level, seed, low=False):
    rng = random.Random(seed)
    previous = 0.0
    out = []
    for i in range(round(duration * RATE)):
        value = rng.uniform(-1, 1)
        if low:
            previous = 0.92 * previous + 0.08 * value
            value = previous * 3
        out.append(level * envelope(i / RATE, duration) * value)
    return out


def mix(*parts):
    out = [0.0] * max(len(p) for p in parts)
    for part in parts:
        for i, value in enumerate(part):
            out[i] += value
    return out


def delay(part, seconds):
    return [0.0] * round(seconds * RATE) + part


def notes(pitches, step=0.07, level=0.5):
    return mix(*(delay(tone(440 * 2 ** ((note - 69) / 12), step * 1.8, level),
                       index * step) for index, note in enumerate(pitches)))


def slice_hit():
    """60 ms unpitched peel snap with a short, softer pulp texture.

    Independent seeded noise is band-limited for the 8 kHz bank. Two cascaded
    1.4 kHz low-pass stages soften the Nyquist edge; subtracting a 220 Hz
    low-pass avoids spending a small speaker's headroom on low rumble. No
    oscillator, pitch sweep, reverb or external recording is involved.
    """
    duration = 0.060
    rng = random.Random(0x534C4943)
    alpha = 1.0 - math.exp(-2.0 * math.pi * 1400.0 / RATE)
    low_alpha = 1.0 - math.exp(-2.0 * math.pi * 220.0 / RATE)
    low = first = second = 0.0
    out = []
    for i in range(round(duration * RATE)):
        t = i / RATE
        value = rng.uniform(-1.0, 1.0)
        first += alpha * (value - first)
        second += alpha * (first - second)
        low += low_alpha * (second - low)
        band = second - low
        snap = 0.90 * math.exp(-t / 0.0036)
        pulp = 0.70 * (1.0 - math.exp(-t / 0.0012)) * math.exp(-t / 0.012)
        grains = sum(gain * math.exp(-((t - center) / 0.0007) ** 2)
                     for center, gain in ((0.007, 0.22), (0.012, 0.14), (0.020, 0.08)))
        edge = min(1.0, t / 0.00075) * min(1.0, (duration - t) / 0.006)
        out.append(1.9 * band * (snap + pulp + grains) * edge)
    return out


def impact(t, start_ms, attack_ms, decay_ms):
    age = t - start_ms
    return (1.0 - math.exp(-age / attack_ms)) * math.exp(-age / decay_ms) if age > 0 else 0.0


def cut_noise(seed, duration_ms, center_hz, q, *bursts):
    """Deterministic band-pass noise shaped by (start, attack, decay, gain) bursts."""
    rng = random.Random(seed)
    omega = 2.0 * math.pi * center_hz / RATE
    alpha = math.sin(omega) / (2.0 * q)
    b0 = alpha / (1.0 + alpha)
    a1 = -2.0 * math.cos(omega) / (1.0 + alpha)
    a2 = (1.0 - alpha) / (1.0 + alpha)
    x1 = x2 = y1 = y2 = 0.0
    out = []
    for i in range(round(duration_ms * RATE / 1000)):
        t = i * 1000 / RATE
        x = rng.uniform(-1.0, 1.0)
        y = b0 * (x - x2) - a1 * y1 - a2 * y2
        x1, x2, y1, y2 = x, x1, y, y1
        contour = sum(gain * impact(t, start, attack, decay)
                      for start, attack, decay, gain in bursts)
        out.append(y * contour * min(1.0, (duration_ms - t) / 5.0))
    return out


def weapon_hits():
    # Jade: a bright, narrow cut that opens from a wet edge into a fast sheen.
    jade = mix(cut_noise(31, 50, 1200, 1.0, (0, 0.55, 4.5, 0.59)),
               cut_noise(32, 50, 2400, 1.85, (1, 3.2, 7.5, 0.70)),
               cut_noise(33, 50, 760, 1.0, (6, 2.3, 9, 0.38)))
    # Flame: meaty impact with a damped low-mid knock and a short pulp crunch.
    flame = mix(cut_noise(41, 48, 660, 0.93, (0, 1.2, 10, 0.76)),
                cut_noise(42, 48, 1250, 0.9, (0, 0.7, 6, 0.43), (10, 1.8, 5, 0.25)),
                tone(395, 0.027, 0.32, -2600))
    # Bamboo: two separated, dry knocks, with different wood resonances.
    bamboo = mix(cut_noise(51, 38, 1450, 2.2, (0, 0.45, 3.6, 0.91)),
                 cut_noise(52, 38, 840, 2.0, (0, 0.8, 5.7, 0.54), (14, 0.7, 4.6, 0.80)),
                 cut_noise(53, 38, 2200, 1.8, (14, 0.55, 2.4, 0.47)))
    # Frost: short rind impact followed by small, staggered ice fractures.
    frost = mix(cut_noise(61, 60, 950, 1.2, (0, 0.55, 4.3, 0.87)),
                cut_noise(62, 60, 1900, 3.7, (3, 0.85, 5.0, 0.60), (22, 0.8, 4, 0.49)),
                cut_noise(63, 60, 2630, 3.0, (11, 0.75, 4, 0.62), (32, 0.7, 4, 0.38)),
                cut_noise(64, 60, 580, 1.0, (1, 1.5, 8, 0.30)))
    # Storm: a fast front-loaded crack with close, irregular secondary sparks.
    storm = mix(cut_noise(71, 40, 1780, 0.86, (0, 0.42, 2.7, 1.08),
                          (4.2, 0.38, 2.4, 0.83), (8.5, 0.45, 3.3, 0.56)),
                cut_noise(72, 40, 570, 1.0, (0.8, 0.7, 6, 0.74)),
                cut_noise(73, 40, 1110, 1.35, (9, 0.8, 6, 0.52)))
    return tuple([sample * gain for sample in clip]
                 for clip, gain in ((jade, 2.7), (flame, 1.75), (bamboo, 2.7),
                                    (frost, 2.7), (storm, 1.6)))


def cheer():
    # Harmonic source filtered by three moving vowel formants: /w/ -> /u:/.
    duration = 0.42
    phase = 0.0
    out = []
    for i in range(round(duration * RATE)):
        t = i / RATE
        f0 = 240 + 110 * math.sin(math.pi * t / duration)
        phase += 2 * math.pi * f0 / RATE
        formants = (350, 750 + 250 * min(1, t / 0.09), 2450)
        value = 0.0
        for harmonic in range(1, 16):
            f = harmonic * f0
            gain = sum(a * math.exp(-0.5 * ((f - center) / width) ** 2)
                       for center, width, a in zip(formants, (140, 180, 260), (1, 0.6, 0.2)))
            value += math.sin(phase * harmonic) * gain / harmonic
        out.append(0.7 * envelope(t, duration, 0.035) * value)
    return out


def music():
    # Original 4-bar, 120 BPM C-Am-F-G arcade ostinato; exactly 8 seconds.
    out = [0.0] * (RATE * 8)
    melody = (72, 76, 79, 76, 74, 76, 79, 81,
              76, 81, 84, 81, 79, 76, 74, 72,
              77, 81, 84, 81, 79, 77, 76, 74,
              79, 83, 86, 83, 81, 79, 74, 71)
    for index, note in enumerate(melody):
        part = tone(440 * 2 ** ((note - 69) / 12), 0.21, 0.18)
        start = index * RATE // 4
        for i, v in enumerate(part):
            out[(start + i) % len(out)] += v
    for beat in range(16):
        root = (48, 45, 41, 43)[beat // 4]
        bass = tone(440 * 2 ** ((root - 69) / 12), 0.35, 0.22)
        kick = tone(100, 0.12, 0.17, -450)
        hat = noise(0.035, 0.08, beat + 40)
        for part, start in ((bass, beat * RATE // 2), (kick, beat * RATE // 2),
                            (hat, beat * RATE // 2 + RATE // 4)):
            for i, v in enumerate(part):
                out[(start + i) % len(out)] += v
    # Every voice decays before the exact bar boundary: no loop discontinuity.
    return out


def bank():
    return dict(zip(NAMES, (
        noise(0.075, 0.5, 1),
        slice_hit(),
        notes((76, 79, 84), 0.065, 0.5),
        mix(noise(0.42, 1.0, 3, True), tone(130, 0.38, 0.65, -270)),
        tone(370, 0.17, 0.45, -1100),
        notes((55, 51, 48), 0.15, 0.45),
        notes((60, 64, 67, 72), 0.09, 0.45),
        notes((88, 95), 0.055, 0.4),
        notes((72, 76, 79, 84), 0.09, 0.5),
        notes((67, 72, 79), 0.06, 0.45),
        mix(noise(0.055, 0.25, 4), tone(880, 0.13, 0.5)),
        notes((72, 79, 84), 0.10, 0.4),
        notes((72, 79, 84, 88, 91), 0.10, 0.5),
        cheer(), *weapon_hits(), music())))


def generate(destination, previews=None):
    clips = bank()
    lines = ['/* Generated by resources/generate_audio.py; see resources/README.md. */',
             '#include "h2_fruit_slice_audio_bank.h"', '', '/* clang-format off */']
    manifest = {'sample_rate_hz': RATE, 'encoding': 'signed PCM8', 'clips': {}}
    for name, values in clips.items():
        # Reserve mix headroom; signed PCM8 is expanded into S16 at playback.
        peak = max(abs(x) for x in values) or 1
        scale = min(0.82 / peak, 1.0) * 127
        samples = [max(-127, min(127, round(x * scale))) for x in values]
        raw = bytes(x & 255 for x in samples)
        manifest['clips'][name] = {'samples': len(samples), 'bytes': len(raw),
                                   'sha256': hashlib.sha256(raw).hexdigest()}
        lines.append(f'static const int8_t {name}[] = {{')
        for start in range(0, len(samples), 24):
            lines.append('    ' + ', '.join(str(x) for x in samples[start:start + 24]) + ',')
        lines.append('};\n')
        if previews:
            previews.mkdir(parents=True, exist_ok=True)
            with wave.open(str(previews / f'{name}.wav'), 'wb') as output:
                output.setparams((1, 1, RATE, 0, 'NONE', 'not compressed'))
                output.writeframes(bytes(x + 128 for x in samples))
    lines.append('const h2_fruit_slice_audio_clip_t h2_fruit_slice_audio_bank[] = {')
    lines.extend(f'    {{{name}, sizeof({name})}},' for name in NAMES)
    lines.append('};\n/* clang-format on */\n')
    destination.write_text('\n'.join(lines))
    (ROOT / 'audio_manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'{sum(v["bytes"] for v in manifest["clips"].values())} PCM bytes; {len(clips)} clips')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--preview-dir', type=Path)
    args = parser.parse_args()
    generate(ROOT.parent / 'src/h2_fruit_slice_audio_bank.c', args.preview_dir)
