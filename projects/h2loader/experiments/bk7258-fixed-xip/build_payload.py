#!/usr/bin/env python3
"""Build a standalone AP program at an explicit BK Flash XIP address.

Produces raw and 32+2 CRC images. Does not access or program hardware.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ (0x8005 if crc & 0x8000 else 0)) & 0xFFFF
    return crc


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--toolchain', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    physical = 3808 * 1024
    assert physical % 34 == 0
    base = 0x02000000 + physical // 34 * 32
    source = Path(__file__).with_name('payload.c').resolve()
    linker = out / 'payload.ld'
    linker.write_text(f'''ENTRY(payload_entry)
SECTIONS {{
 . = 0x{base:08x};
 .header : {{ LONG(0x58324648); LONG(payload_entry + 1); LONG(0x{base:08x}); LONG(1); }}
 . = 0x{base + 256:08x};
 .text : {{ KEEP(*(.entry)) *(.text*) *(.rodata*) *(.data*) }}
 /DISCARD/ : {{ *(.comment) *(.ARM.exidx*) *(.ARM.extab*) }}
}}
''')
    def run(tool, *argv):
        subprocess.run([str(args.toolchain / ('arm-none-eabi-' + tool)),
                        *map(str, argv)], check=True)
    run('gcc', '-mcpu=cortex-m33', '-mthumb', '-Os', '-ffreestanding',
        '-fno-builtin', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
        '-nostdlib', '-Wl,--build-id=none', f'-Wl,-T,{linker}',
        f'-Wl,-Map,{out / "payload.map"}', source, '-o', out / 'payload.elf')
    run('objcopy', '-O', 'binary', '--gap-fill', '0xff', out / 'payload.elf', out / 'payload.bin')
    raw = (out / 'payload.bin').read_bytes()
    raw += b'\xff' * (-len(raw) % 32)
    encoded = b''.join(raw[i:i+32] + struct.pack('>H', crc16(raw[i:i+32]))
                       for i in range(0, len(raw), 32))
    (out / 'payload.crc.bin').write_bytes(encoded)
    manifest = dict(physical_offset=hex(physical), virtual_base=hex(base),
                    entry=hex(struct.unpack_from('<I', raw, 4)[0]),
                    crc_image_size=len(encoded), sha256=hashlib.sha256(encoded).hexdigest())
    (out / 'payload.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest))


if __name__ == '__main__':
    main()
