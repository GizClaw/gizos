#!/usr/bin/env python3
"""Encode original tones and retain their raw AAC access units, not ADTS."""

from pathlib import Path
import argparse
import hashlib
import json
import math
import struct
import subprocess
import tempfile


def encode(executable, name, rate, tones, directory):
    pcm = directory / (name + ".s16le")
    adts = directory / (name + ".aac")
    samples = bytearray()
    for index in range(12 * 1024):
        for channel, frequency in enumerate(tones):
            amplitude = 0.12 if channel == 0 else 0.08
            value = round(32767 * amplitude * math.sin(2 * math.pi * frequency * index / rate))
            samples += struct.pack("<h", value)
    pcm.write_bytes(samples)
    command = [executable, "-hide_banner", "-loglevel", "error", "-y",
               "-f", "s16le", "-ar", str(rate), "-ac", str(len(tones)),
               "-i", str(pcm), "-c:a", "aac", "-profile:a", "aac_low",
               "-b:a", "96k", "-f", "adts", str(adts)]
    subprocess.run(command, check=True)
    encoded = adts.read_bytes()
    packets = []
    offset = 0
    expected_config = None
    while offset < len(encoded):
        header = encoded[offset:offset + 7]
        assert len(header) == 7 and header[0] == 255 and header[1] & 0xF6 == 0xF0
        header_size = 7 if header[1] & 1 else 9
        object_type = (header[2] >> 6) + 1
        rate_index = (header[2] >> 2) & 15
        channels = ((header[2] & 1) << 2) | (header[3] >> 6)
        length = ((header[3] & 3) << 11) | (header[4] << 3) | (header[5] >> 5)
        assert object_type == 2 and channels == len(tones)
        assert header[6] & 3 == 0 and length >= header_size
        assert offset + length <= len(encoded)
        config = struct.pack(">H", (object_type << 11) | (rate_index << 7) | (channels << 3))
        assert expected_config in (None, config)
        expected_config = config
        packets.append(encoded[offset + header_size:offset + length])
        offset += length
    assert packets and offset == len(encoded)
    return packets, expected_config, hashlib.sha256(encoded).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    args = parser.parse_args()
    destination = Path(__file__).resolve().parent
    source = ['/* Generated original AAC-LC tones; see fixtures/README.md. */',
              '#include "h2_aac_vectors.h"', ""]
    details = []
    version = subprocess.check_output([args.ffmpeg, "-version"], text=True).splitlines()[0]
    with tempfile.TemporaryDirectory(prefix="pal-aac-fixture-") as temporary:
        for name, rate, tones in [("mono", 44100, [997]), ("stereo", 48000, [997, 1999])]:
            packets, config, digest = encode(args.ffmpeg, name, rate, tones, Path(temporary))
            for index, packet in enumerate(packets):
                source.append(f"static const uint8_t {name}_{index}[] = {{")
                for offset in range(0, len(packet), 16):
                    source.append("    " + ", ".join(f"0x{byte:02x}" for byte in packet[offset:offset + 16]) + ",")
                source.append("};")
            source.append(f"static const h2_aac_e2e_packet_t {name}_packets[] = {{")
            source += [f"    {{{name}_{i}, sizeof({name}_{i})}}," for i in range(len(packets))]
            source.append("};")
            frequencies = tones + [0] * (2 - len(tones))
            source += [f"const h2_aac_e2e_vector_t h2_aac_e2e_{name} = {{",
                       f'    .name = "{name}", .sample_rate_hz = {rate}u, .channels = {len(tones)}u,',
                       f"    .audio_specific_config = {{0x{config[0]:02x}, 0x{config[1]:02x}}},",
                       f"    .tone_hz = {{{frequencies[0]}u, {frequencies[1]}u}},",
                       f"    .packets = {name}_packets, .packet_count = {len(packets)}u,", "};", ""]
            details.append(f"- `{name}`: {rate} Hz, {len(tones)} channel(s), tones {tones} Hz, "
                           f"{len(packets)} raw AAC packets, ASC `{config.hex()}`, ADTS SHA-256 `{digest}`.")
    output = destination / "h2_aac_vectors.c"
    output.write_text("\n".join(source))
    documentation = ("# Original AAC-LC fixtures\n\n"
                     "These are original generated sine waves, encoded by the command in\n"
                     "`generate_vectors.py`. No third-party song or media is used.\n\n"
                     f"Encoder: `{version}`. The committed bytes are the stable test input;\n"
                     "regeneration with a different encoder version may change them.\n\n" +
                     "\n".join(details) + "\n\n"
                     "ADTS framing is removed before embedding. The PAL receives raw AAC-LC\n"
                     "access units and a separate AudioSpecificConfig. PCM checks allow codec\n"
                     "rounding and encoder priming while requiring nonzero signal, correct\n"
                     "format, channel separation and the original tone frequencies.\n\n"
                     f"Generated C SHA-256: `{hashlib.sha256(output.read_bytes()).hexdigest()}`.\n")
    (destination / "README.md").write_text(documentation)
    print(json.dumps({"source": str(output), "encoder": version,
                      "source_sha256": hashlib.sha256(output.read_bytes()).hexdigest()}))


if __name__ == "__main__":
    main()
