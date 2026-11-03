"""Embed H202 MFG prompts as 4-bit IMA ADPCM C arrays.

Usage: ima_adpcm.py OUTPUT.c NAME=PCM_PATH...

Each input is raw 8 kHz mono PCM16LE. Leading and trailing near-silence is
trimmed to a short margin, then the samples are encoded as one IMA ADPCM
stream: predictor and step index start at 0, the low nibble holds the earlier
sample, and an odd count is padded with one silent sample. The decoder is
h2_h202_mfg_adpcm_next() in app/src/h2_h202_mfg.c; keep both in step.
"""

import pathlib
import re
import struct
import sys

STEPS = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209,
    230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
    963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749,
    3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767)
INDEX = (-1, -1, -1, -1, 2, 4, 6, 8)
SILENCE = 256  # |sample| at or below this counts as silence when trimming.
MARGIN = 640  # 80 ms of the original edge kept on each side.


def step(state, nibble):
    """Apply one nibble to (predictor, index); returns the new state."""
    predictor, index = state
    size = STEPS[index]
    diff = size >> 3
    if nibble & 4:
        diff += size
    if nibble & 2:
        diff += size >> 1
    if nibble & 1:
        diff += size >> 2
    predictor = predictor - diff if nibble & 8 else predictor + diff
    predictor = max(-32768, min(32767, predictor))
    index = max(0, min(88, index + INDEX[nibble & 7]))
    return predictor, index


def encode(samples):
    if len(samples) % 2:
        samples = list(samples) + [0]
    state = (0, 0)
    nibbles = []
    for sample in samples:
        predictor, index = state
        size = STEPS[index]
        delta = sample - predictor
        nibble = 8 if delta < 0 else 0
        delta = abs(delta)
        if delta >= size:
            nibble |= 4
            delta -= size
        if delta >= size >> 1:
            nibble |= 2
            delta -= size >> 1
        if delta >= size >> 2:
            nibble |= 1
        state = step(state, nibble)
        nibbles.append(nibble)
    return bytes(nibbles[i] | nibbles[i + 1] << 4
                 for i in range(0, len(nibbles), 2))


def decode(data):
    state = (0, 0)
    samples = []
    for byte in data:
        for nibble in (byte & 15, byte >> 4):
            state = step(state, nibble)
            samples.append(state[0])
    return samples


def trim(samples):
    loud = [i for i, sample in enumerate(samples) if abs(sample) > SILENCE]
    if not loud:
        raise ValueError("prompt is silent")
    return samples[max(0, loud[0] - MARGIN):loud[-1] + 1 + MARGIN]


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    text = ["#include <stddef.h>\n"]
    for spec in argv[1:]:
        name, _, path = spec.partition("=")
        if not re.fullmatch(r"[a-z][a-z0-9_]*", name) or not path:
            raise SystemExit(f"invalid prompt spec: {spec}")
        raw = pathlib.Path(path).read_bytes()
        if not raw or len(raw) % 2:
            raise SystemExit(f"not whole PCM16 samples: {path}")
        samples = trim(list(struct.unpack(f"<{len(raw) // 2}h", raw)))
        data = encode(samples)
        body = ",".join(f"0x{byte:02x}" for byte in data)
        text.append(f"const unsigned char h2_h202_prompt_{name}[] = {{{body}}};\n")
        text.append(f"const size_t h2_h202_prompt_{name}_size = "
                    f"sizeof(h2_h202_prompt_{name});\n")
    output = pathlib.Path(argv[0])
    content = "".join(text)
    if not output.exists() or output.read_text() != content:
        output.write_text(content)


if __name__ == "__main__":
    main(sys.argv[1:])
