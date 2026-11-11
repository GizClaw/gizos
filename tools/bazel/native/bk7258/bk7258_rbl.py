"""Write the ordinary BK7258 paired-image RBL format used by native builds."""

import argparse
import json
import os
from pathlib import Path
import struct
import sys
import zlib


FOOTER_SIZE = 4096
MAX_IMAGE_SIZE = 16 * 1024 * 1024


def partition_size(value):
    text = str(value).strip().upper()
    multiplier = 1
    if text.endswith(("K", "M")):
        multiplier = 1024 if text[-1] == "K" else 1024 * 1024
        text = text[:-1]
    size = int(text, 16 if text.startswith("0X") else 10) * multiplier
    if size <= FOOTER_SIZE or size > MAX_IMAGE_SIZE:
        raise ValueError("invalid RBL partition size")
    return size


def pack(source, output, metadata, *, firmware="app", version="2"):
    sections = json.loads(metadata.read_text())["section"]
    if (not isinstance(sections, list) or len(sections) != 2
            or not isinstance(sections[1], dict)
            or sections[1].get("partition") != "app"):
        raise ValueError("RBL requires paired-image partition metadata")
    capacity = partition_size(sections[1]["size"])
    size = source.stat().st_size
    if not size or size >= capacity - FOOTER_SIZE:
        raise ValueError("RBL image must leave room for the native footer")
    name, release = firmware.encode(), version.encode()
    if not name or len(name) > 16 or not release or len(release) > 24:
        raise ValueError("RBL firmware identity exceeds its native field")
    data = source.read_bytes()
    if len(data) != size:
        raise ValueError("RBL image changed while being read")
    checksum = zlib.crc32(data)
    hash_value = 0x811C9DC5
    for byte in data:
        hash_value = ((hash_value ^ byte) * 0x1000193) & 0xFFFFFFFF
    header = struct.pack(
        "<4sH2xI16s24s24sIIII",
        b"RBL\0", 0, int(source.stat().st_mtime), name, release,
        b"00010203040506070809", checksum, hash_value, size, size,
    )
    header += struct.pack("<I", zlib.crc32(header))
    with output.open("wb") as stream:
        stream.write(data)
        stream.write(b"\xff" * (capacity - size))
        stream.seek(capacity - FOOTER_SIZE)
        stream.write(header)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-i", "--infile", type=Path, required=True)
    parser.add_argument("-o", "--outfile", type=Path, required=True)
    parser.add_argument("-pjd", type=Path, required=True)
    parser.add_argument("-soc", default="bk7258")
    parser.add_argument("-ap")
    parser.add_argument("-g")
    parser.add_argument("-fn", "--firmware", default="app")
    parser.add_argument("-fv", "--firmwareversion", default="2")
    parser.add_argument("command", choices=["packager"])
    args = parser.parse_args()
    try:
        config_path = args.pjd / "partitions" / args.soc / "ota_rbl.config"
        if config_path.exists():
            config = json.loads(config_path.read_text())
            if (not isinstance(config, dict) or set(config) - {"gzip", "outf"}
                    or str(config.get("gzip", "0")) != "0"):
                raise ValueError("unsupported RBL packaging configuration")
        metadata = Path(os.environ["PARTITIONS_DIR"]) / "configurationab.json"
        pack(args.infile, args.outfile, metadata,
             firmware=args.firmware, version=args.firmwareversion)
    except (OSError, ValueError, KeyError, TypeError, struct.error) as error:
        print(f"RBL packaging failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
