"""Emit a fixed-size flash constant from the prepared plant span archive."""

from pathlib import Path
import sys


def main():
    binary, output = map(Path, sys.argv[1:])
    payload = binary.read_bytes()
    with output.open("w", encoding="ascii") as stream:
        stream.write('#include <stdint.h>\n\n')
        stream.write('extern const uint8_t h2_garden_plant_spans_archive[];\n')
        stream.write('const uint8_t h2_garden_plant_spans_archive[] = {\n')
        for start in range(0, len(payload), 24):
            stream.write('    ' + ''.join(f'0x{byte:02x},' for byte in payload[start:start + 24]) + '\n')
        stream.write('};\n')


if __name__ == "__main__":
    main()
