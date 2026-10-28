"""Validate a BK AP register/stack coredump captured by the native fatal hook."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import zlib

REGISTERS = tuple([f'r{i}' for i in range(13)] + ['sp', 'lr', 'pc', 'xpsr', 'msp', 'psp', 'primask', 'basepri', 'faultmask', 'control', 'fpscr'])


def decode(data):
    if len(data) < 256 or data[4:12] != b'H2BKDMP1':
        raise ValueError('missing BK coredump header')
    word = lambda offset: struct.unpack_from('<I', data, offset)[0]
    if not 256 <= word(0) <= 4416 or word(12) not in (1, 2) or word(28) != 256 or word(252) != 0x43444F4E:
        raise ValueError('unsupported or incomplete BK coredump header')
    if word(248) != zlib.crc32(data[:248]):
        raise ValueError('register coredump CRC mismatch')
    if word(12) == 2 and word(144) != 2:
        raise ValueError('unsupported coredump processor')
    processor = 'cp' if word(12) == 2 else 'ap'
    regs = dict(zip(REGISTERS, struct.unpack_from('<24I', data, 32)))
    result = dict(format='H2BKDMP1', processor=processor, declared_bytes=word(0), core=word(16), reason=word(20), registers=regs,
                  cfsr=word(128), hfsr=word(132), bfar=word(136), mmfar=word(140),
                  registers_valid=True, stack_valid=False, stack_bytes=0)
    if len(data) >= 288 and data[256:264] == b'H2STACK1':
        address, size, crc = struct.unpack_from('<III', data, 264)
        header_valid = word(280) == zlib.crc32(data[256:280]) and word(284) == 0x53544F4E
        if header_valid and 0 < size <= 4096 and size % 32 == 0 and address == regs['sp'] and len(data) >= 288 + size + 32:
            stack = data[288:288 + size]
            if zlib.crc32(stack) == crc and data[288 + size:296 + size] == b'H2END001':
                result.update(stack_valid=True, stack_address=address, stack_bytes=size,
                              stack_words=list(struct.unpack('<' + str(size // 4) + 'I', stack)))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump', type=Path)
    parser.add_argument('--elf', type=Path)
    parser.add_argument('--addr2line', default='arm-none-eabi-addr2line')
    args = parser.parse_args()
    result = decode(args.dump.read_bytes())
    if args.elf:
        pcs = [result['registers'][r] for r in ['pc', 'lr']]
        result['symbols'] = subprocess.check_output([args.addr2line, '-f', '-C', '-e', str(args.elf), *[hex(pc & ~1) for pc in pcs]], text=True).splitlines()
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
