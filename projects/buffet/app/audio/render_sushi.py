#!/usr/bin/env python3
"""Render Lanterns on the Belt and its cues. Python 3.9+, standard library only."""

import argparse
from array import array
from math import exp, log10, pi, sin, sqrt, tau
from pathlib import Path
from random import Random
import sys
import wave

RATE = 24_000
BEAT = 12_800  # 112.5 BPM, exactly 12800 samples per quarter at 24 kHz.
FULL_SCORE = tuple(range(24))
# Four bars each of A, B, and the returning variation. The tonic downbeat
# follows the same dominant turnaround as the full arrangement.
COMPACT_SCORE = (0, 1, 2, 3, 8, 9, 14, 15, 16, 19, 22, 23)
NOTE_NAMES = 'C C# D D# E F F# G G# A A# B'.split()

# Harmony for A / B / A'. Each row is bass root, bass alternate, and pluck voicing.
HARMONY = [
    ('D2 A2 F#3 A3 C#4 E4'), ('C#2 E3 A3 B3 C#4 E4'),
    ('B1 F#2 F#3 A3 C#4 D4'), ('G2 D3 F#3 A3 B3 D4'),
    ('E2 B2 G3 B3 D4 F#4'), ('F#2 C#3 A3 C#4 E4 A4'),
    ('G2 D3 B3 D4 F#4 A4'), ('A2 E3 G3 B3 D4 E4'),
    ('B1 F#2 F#3 A3 C#4 D4'), ('A2 E3 A3 C#4 E4 F#4'),
    ('G2 D3 F#3 A3 B3 D4'), ('F#2 C#3 A3 C#4 E4 G#4'),
    ('E2 B2 G3 B3 D4 F#4'), ('F#2 C#3 A3 C#4 E4 F#4'),
    ('G2 D3 B3 D4 F#4 A4'), ('A2 E3 G3 B3 C#4 F#4'),
    ('D2 A2 F#3 A3 C#4 E4'), ('C#2 E3 A3 B3 C#4 E4'),
    ('B1 F#2 F#3 A3 C#4 D4'), ('G2 D3 F#3 A3 B3 D4'),
    ('E2 B2 G3 B3 D4 F#4'), ('F#2 C#3 A3 C#4 E4 A4'),
    ('G2 D3 B3 D4 F#4 A4'), ('A2 E3 G3 B3 C#4 E4'),
]

# Each bar is beat:note:duration in quarter-note units. Fractional placements
# are the swing, pickups and rests for the marimba lead. Bars 9–16 use reed.
MELODY = [
    '.55:F#5:.48 1.12:A5:.71 2.03:B5:.35 2.58:A5:.42 3.12:E5:.64',
    '.03:F#5:.39 .58:E5:.48 1.48:C#5:.64 2.55:B4:.4 3.09:C#5:.62',
    '.54:D5:.47 1.12:F#5:.69 2.07:A5:.4 2.62:F#5:.43 3.25:D5:.59',
    '.05:E5:.47 .63:D5:.6 1.56:B4:.77 3.03:A4:.34 3.57:B4:.38',
    '.14:D5:.56 1.05:F#5:.56 1.87:G5:.42 2.49:F#5:.4 3.07:B4:.69',
    '.02:C#5:.75 1.16:E5:.4 1.76:F#5:.66 2.92:A5:.72',
    '.09:B5:.74 1.12:A5:.38 1.67:F#5:.65 2.57:E5:.46 3.15:D5:.52',
    '.09:E5:.53 .74:F#5:.39 1.52:E5:.64 2.55:A4:.48 3.24:C#5:.63',
    '.40:F#4:1.15 1.72:B4:.73 2.69:C#5:.32 3.18:D5:.72',
    '.06:E5:1.34 1.58:C#5:.72 2.58:B4:.51 3.27:A4:.61',
    '.42:B4:1 1.61:D5:.86 2.64:F#5:.88',
    '.18:E5:1.10 1.43:C#5:.61 2.34:G#4:.76 3.38:A4:.46',
    '.06:B4:.68 .86:G4:.56 1.59:F#4:.74 2.72:G4:.68',
    '.11:A4:.63 .93:C#5:.87 2.08:E5:.57 2.91:F#5:.82',
    '.04:D5:.92 1.19:B4:.71 2.28:A4:1.12 3.62:B4:.36',
    '.05:C#5:1.09 1.48:E5:.67 2.41:F#5:.59 3.33:E5:.55',
    '.55:F#5:.48 1.12:A5:.71 2.03:B5:.35 2.58:A5:.42 3.12:E5:.64',
    '.02:F#5:.42 .57:E5:.55 1.51:C#5:.48 2.03:E5:.39 2.61:C#5:.61',
    '.55:D5:.47 1.12:F#5:.59 1.97:A5:.41 2.57:C#6:.49 3.18:B5:.59',
    '.11:A5:.55 .89:F#5:.66 1.87:E5:.44 2.55:D5:.66 3.40:B4:.43',
    '.18:D5:.51 .92:E5:.41 1.48:F#5:.56 2.48:A5:.57 3.25:G5:.49',
    '.10:F#5:.53 .88:E5:.65 1.88:C#5:.42 2.55:A4:.43 3.14:C#5:.64',
    '.05:D5:.75 1.13:F#5:.47 1.78:E5:.62 2.65:B4:.42 3.22:D5:.56',
    '.10:E5:.92 1.56:B4:.68 2.65:C#5:.78 3.61:E5:.60',
]

# Short answers in the spaces around the main line. Bar numbers are zero-based.
ANSWERS = {
    1: '2.07:E6:.24', 3: '2.31:D6:.42', 5: '2.42:E6:.28',
    7: '1.77:D5:.32 3.66:E6:.55', 8: '.05:D5:.54 1.2:A4:.41',
    9: '2.39:E5:.31', 10: '.05:D5:.67 2.26:A5:.36',
    11: '3.03:E5:.44', 12: '.55:D5:.43 2.04:B4:.50',
    13: '3.66:E6:.70', 14: '2.84:F#5:.49', 15: '1.03:B5:.44',
    17: '3.46:E6:.48', 19: '2.08:D6:.43', 21: '2.29:E6:.43',
    22: '2.17:A5:.32', 23: '2.00:E6:.78',
}


def seconds(duration):
    return round(duration * RATE)


def smooth(time, duration):
    fraction = min(1.0, max(0.0, time / duration))
    return fraction * fraction * (3.0 - 2.0 * fraction)


def edges(index, count, attack, release):
    return smooth(index, attack) * smooth(count - 1 - index, release)


def pitch(note):
    midi = (int(note[-1]) + 1) * 12 + NOTE_NAMES.index(note[:-1])
    return 440.0 * 2 ** ((midi - 69) / 12)


def add(track, samples, start, gain=1.0, circular=False):
    if circular:
        first = start % len(track)
        for i, sample in enumerate(samples):
            track[(first + i) % len(track)] += gain * sample
    else:
        if start < 0 or start + len(samples) > len(track):
            raise ValueError('Cue event falls outside its render buffer')
        for i, sample in enumerate(samples):
            track[start + i] += gain * sample


def partials(frequency, duration, spec, attack=.006, release=.075):
    """(ratio, gain, decay seconds) resonances with smooth note edges."""
    count = seconds(duration)
    waves = [(tau * frequency * ratio / RATE, weight, seconds(decay))
             for ratio, weight, decay in spec if frequency * ratio < RATE * .45]
    attack_frames, release_frames = seconds(attack), seconds(release)
    return [edges(i, count, attack_frames, release_frames) *
            sum(weight * exp(-i / decay) * sin(i * step) for step, weight, decay in waves)
            for i in range(count)]


def voice(kind, note, duration):
    f = pitch(note)
    if kind == 'mallet':
        return partials(f, duration,
                        [(1, .82, .215), (2.0, .12, .064), (3.99, .32, .036),
                         (8.73, .095, .014), (2.71, .090, .019)], .005, .125)
    if kind == 'reply':
        return partials(f, duration,
                        [(1, .70, .29), (2.001, .085, .12), (3.005, .10, .10),
                         (5.06, .045, .045)], .010, .135)
    if kind == 'pluck':
        return partials(f, duration,
                        [(1, .70, .202), (2.001, .24, .118), (3.003, .12, .076),
                         (4.005, .075, .051), (5.009, .052, .039), (7.01, .03, .020)],
                        .008, .092)
    if kind == 'bass':
        return partials(f, duration,
                        [(1, .62, .285), (2.003, .37, .182), (3.003, .21, .093),
                         (4.008, .08, .068), (5.016, .055, .039)], .010, .084)
    if kind == 'reed':
        count = seconds(duration)
        result = []
        for i in range(count):
            t = i / RATE
            phase = tau * f * t + .083 * sin(tau * 4.9 * t)
            swell = edges(i, count, seconds(.105), seconds(.123))
            body = .72 * sin(phase) + .127 * sin(2 * phase) + .085 * sin(3 * phase)
            result.append(swell * (.83 + .06 * sin(tau * 3.5 * t)) * body)
        return result
    raise ValueError(f'Unknown voice {kind}')


def noise(count, seed, low_hz, high_hz):
    """Deterministic band-shaped white noise from two single-pole filters."""
    random = Random(seed)
    low_alpha = 1 - exp(-tau * low_hz / RATE)
    high_alpha = 1 - exp(-tau * high_hz / RATE)
    low = high = 0.0
    result = []
    for _ in range(count):
        white = random.uniform(-1, 1)
        low += low_alpha * (white - low)
        high += high_alpha * (white - high)
        result.append(high - low)
    return result


def puff(duration, seed, low_hz, high_hz, decay, attack=.004, release=.017):
    count = seconds(duration)
    return [sample * exp(-i / seconds(decay)) * edges(i, count, seconds(attack), seconds(release))
            for i, sample in enumerate(noise(count, seed, low_hz, high_hz))]


def drum():
    count = seconds(.154)
    return [edges(i, count, seconds(.008), seconds(.040)) * exp(-i / seconds(.045)) *
            sin(tau * (57 * (i / RATE) + .86 * (1 - exp(-i / seconds(.018)))))
            for i in range(count)]


def music(score=FULL_SCORE):
    frames = len(score) * 4 * BEAT
    dry = array('d', [0.0]) * frames
    room_send = array('d', [0.0]) * frames
    phrasing = Random(48117)

    def event(bar, beat, kind, note, duration, level, send=0.16):
        samples = voice(kind, note, duration * BEAT / RATE)
        position = round((bar * 4 + beat) * BEAT)
        level *= phrasing.uniform(.91, 1.07)
        add(dry, samples, position, level, circular=True)
        if send:
            add(room_send, samples, position, level * send, circular=True)

    def perform(bar, notes, kind, level, send):
        for index, item in enumerate(notes.split()):
            beat, note, duration = item.split(':')
            event(bar, float(beat), kind, note, float(duration),
                  level * (1.0 if index % 3 == 0 else .83), send)

    soft_drum = drum()
    for slot, bar in enumerate(score):
        root, alternate, *color = HARMONY[bar].split()
        section_b = 8 <= bar < 16
        event(slot, .015, 'bass', root, 1.39, .126, 0)
        event(slot, 1.69, 'bass', root, .42, .074, 0)
        event(slot, 2.55, 'bass', alternate, .69, .108, 0)
        if bar % 4 in (1, 3):
            event(slot, 3.59, 'bass', alternate, .47, .073, 0)

        # Voicings are rolled by roughly 13 ms from low to high, with light
        # varying accents on the off-beats. B leaves more air around the reed.
        for beat, order, amount, duration in [
            (.26, (0, 2, 1, 3), .048, 1.13),
            (1.58, (1, 2, 3), .047, .53),
            (2.16, (0, 1, 3, 2), .042, 1.02),
            (3.55, (1, 3, 2), .042, .74),
        ]:
            for rank, index in enumerate(order):
                event(slot, beat + rank * .025, 'pluck', color[index], duration,
                      amount * (.80 if section_b else 1.0) * (1.0 - rank * .09))

        perform(slot, MELODY[bar], 'reed' if section_b else 'mallet',
                .093 if section_b else .176, .28 if section_b else .23)
        perform(slot, ANSWERS.get(bar, ''), 'reply', .059 if section_b else .038, .44)

        for beat, level in [(0, .052), (2.52, .043)]:
            add(dry, soft_drum, round((slot * 4 + beat) * BEAT), level, circular=True)
        for idx, (beat, level) in enumerate([(0.57, .030), (1.07, .060), (1.62, .023),
                                              (2.57, .027), (3.07, .053), (3.68, .026)]):
            start = round((slot * 4 + beat) * BEAT)
            brush = puff(.104 if idx in (1, 4) else .060, 3000 + bar * 15 + idx,
                         1600, 6550, .028, .009)
            add(dry, brush, start, level, circular=True)
            add(room_send, brush, start, level * .18, circular=True)

    # Small mono ambience. Delay taps and damping wrap as well as note tails.
    # This renders the steady state at the boundary without a seam crossfade.
    smoothed = room_send[-1]
    for index, sample in enumerate(room_send):
        smoothed += .20 * (sample - smoothed)
        room_send[index] = smoothed
    for delay, gain in [(29, .48), (49, .35), (77, .30), (111, -.14),
                        (153, .19), (211, .13), (297, .07)]:
        add(dry, room_send, round(delay * RATE / 1000), gain, circular=True)
    return dry


def plate():
    track = array('d', [0.0]) * seconds(.225)
    # Two ceramic contacts, with softer overtones on the bounce.
    for start, f, level in [(0, 1044, .83), (.039, 1111, .35)]:
        hit = partials(f, .160,
                       [(1, .46, .042), (1.51, .37, .033), (2.37, .31, .026),
                        (3.23, .16, .018), (4.28, .09, .011)], .0018, .046)
        add(track, hit, seconds(start), level)
    add(track, puff(.085, 903, 950, 6600, .016, .0014), 0, .52)
    return track


def bite():
    track = array('d', [0.0]) * seconds(.205)
    # Uneven close crunch, then a small rounded mouth sound.
    for start, length, seed, level, low, high in [
        (0, .061, 51, .73, 570, 5800), (.017, .060, 97, .42, 830, 5100),
        (.034, .078, 181, .80, 390, 4700), (.074, .096, 413, .40, 260, 2500),
    ]:
        add(track, puff(length, seed, low, high, .023, .004, .019), seconds(start), level)
    body = partials(164, .102, [(1, .47, .032), (2.02, .22, .025), (3.05, .16, .017)],
                    .006, .024)
    add(track, body, seconds(.043), .33)
    return track


def red_zone():
    track = array('d', [0.0]) * seconds(.530)
    # A lower-register minor-third double call with a quick second articulation.
    for start, note, length, level in [(0, 'F#4', .190, .70), (.151, 'D#4', .351, .92)]:
        count = seconds(length)
        for i in range(count):
            t = i / RATE
            phase = tau * pitch(note) * (t + .0025 * (1 - exp(-t / .043)))
            envelope = edges(i, count, seconds(.012), seconds(.082)) * exp(-t / .21)
            tone = .78 * sin(phase) + .20 * sin(2 * phase) + .10 * sin(3 * phase)
            track[seconds(start) + i] += level * envelope * tone
    return track


def card_flip():
    track = array('d', [0.0]) * seconds(.300)
    count = seconds(.260)
    # Friction swells at mid-turn. Flex flutter and a dry edge tick finish it.
    for i, sample in enumerate(noise(count, 1711, 630, 7060)):
        t = i / RATE
        swell = exp(-((t - .109) / .054) ** 2) * edges(i, count, seconds(.025), seconds(.038))
        track[i] += .58 * sample * swell * (.77 + .23 * sin(tau * 41 * t))
    add(track, puff(.097, 1971, 190, 2460, .032, .006), seconds(.115), .37)
    add(track, puff(.069, 2011, 1200, 7250, .013, .002), seconds(.178), .41)
    return track


def card_reveal():
    track = array('d', [0.0]) * seconds(1.180)
    # Upward fifth and bright sixth on top, with a soft tonic octave underneath.
    for start, note, length, level in [(0, 'D5', .65, .64), (.091, 'A5', .69, .68),
                                        (.184, 'B5', .89, .79), (.184, 'D6', .73, .25)]:
        bell = partials(pitch(note), length,
                        [(1, .79, .335), (2, .21, .125), (2.75, .12, .152),
                         (4.01, .09, .110), (5.94, .06, .064)], .006, .147)
        start_frame = seconds(start)
        add(track, bell, start_frame, level)
        add(track, bell, start_frame + seconds(.052), level * .13)
        add(track, bell, start_frame + seconds(.094), level * .085)
    return track


def admission():
    track = array('d', [0.0]) * seconds(.520)
    # Ticket touch, then a warm two-note pickup into the room.
    add(track, puff(.067, 610, 1200, 5000, .013, .002), 0, .48)
    for start, note, duration, level in [(.015, 'A4', .265, .80), (.110, 'D5', .376, .90)]:
        add(track, voice('mallet', note, duration), seconds(start), level)
    return track


def recover():
    track = array('d', [0.0]) * seconds(.440)
    for start, note, duration, level in [(0, 'D4', .228, .65), (.133, 'F#4', .273, .86)]:
        add(track, voice('pluck', note, duration), seconds(start), level)
    return track


def closing():
    track = array('d', [0.0]) * seconds(.990)
    # A soft descending three-note cadence, with a low tonic underpinning.
    for start, note, duration, level in [(0, 'A5', .42, .64), (.171, 'F#5', .46, .69),
                                         (.353, 'D5', .60, .82), (.353, 'D4', .60, .30)]:
        add(track, voice('reply', note, duration), seconds(start), level)
    return track


RENDERS = {
    'lanterns-on-the-belt.wav': (music, -8.0),
    'plate-pickup.wav': (plate, -7.0),
    'bite.wav': (bite, -10.0),
    'stomach-red.wav': (red_zone, -8.0),
    'card-flip.wav': (card_flip, -9.0),
    'card-reveal.wav': (card_reveal, -7.0),
    'admission.wav': (admission, -9.0),
    'stomach-recover.wav': (recover, -11.0),
    'closing.wav': (closing, -9.0),
}


def export(path, samples, target_peak, loop=False):
    # Remove any DC without introducing an offset on cue endpoints.
    if loop:
        mean = sum(samples) / len(samples)
        samples = [sample - mean for sample in samples]
    else:
        weight = [sin(pi * i / (len(samples) - 1)) ** 2 for i in range(len(samples))]
        mean = sum(samples) / sum(weight)
        samples = [sample - mean * shape for sample, shape in zip(samples, weight)]
    normalize = 32767 * 10 ** (target_peak / 20) / max(map(abs, samples))
    pcm = array('h', (round(sample * normalize) for sample in samples))
    if sys.byteorder != 'little':
        pcm.byteswap()
    with wave.open(str(path), 'wb') as file:
        file.setparams((1, 2, RATE, len(pcm), 'NONE', 'not compressed'))
        file.writeframes(pcm.tobytes())
    return 20 * log10(sqrt(sum(sample * sample for sample in samples) / len(samples))
                      * normalize / 32767)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, help='directory for PCM WAV exports')
    parser.add_argument('--compact-only', action='store_true', help='render the 12-bar loop only')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    renders = {'lanterns-on-the-belt-compact.wav': (lambda: music(COMPACT_SCORE), -8.0)} \
        if args.compact_only else RENDERS
    for name, (render, peak) in renders.items():
        samples = render()
        rms = export(args.output / name, samples, peak, loop=name.startswith('lanterns-'))
        print(f'{name:34} {len(samples) / RATE:6.3f}s  peak {peak:5.1f}  RMS {rms:5.1f} dBFS')


if __name__ == '__main__':
    main()
