#!/usr/bin/env python3

import pathlib
import sys


def main() -> int:
    source = pathlib.Path(sys.argv[1]).read_bytes()
    output = pathlib.Path(sys.argv[2])
    with output.open("w", encoding="utf-8") as stream:
        stream.write("#include <stdint.h>\n\n")
        stream.write(
            "const uint8_t h2_jieli_embedded_startup_mp4[] "
            "__attribute__((aligned(16))) = {\n"
        )
        for offset in range(0, len(source), 16):
            values = ", ".join(f"0x{value:02x}" for value in source[offset:offset + 16])
            stream.write(f"  {values},\n")
        stream.write("};\n")
        stream.write(
            f"const uint32_t h2_jieli_embedded_startup_mp4_size = {len(source)}u;\n"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
