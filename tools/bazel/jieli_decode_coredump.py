#!/usr/bin/env python3
"""Decode a WL82 H2CORE v2 crash record or retained RAM log ring."""

import argparse
import struct
import sys

MAGIC = 0x52433248
VERSION = 2
COMMITTED = 0x54494D43
LOG_CAPACITY = 2048
HEADER_FORMAT = "<10I"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
RECORD_SIZE = HEADER_SIZE + LOG_CAPACITY + struct.calcsize("<2I")
RETAINED_LOG_MAGIC = 0x474F4C48
RETAINED_HEADER_SIZE = struct.calcsize("<3I")
RETAINED_SIZE = RETAINED_HEADER_SIZE + LOG_CAPACITY


def decode_retained_log(data: bytes) -> tuple[bytes, int, int]:
    """Decode retained_log at its ELF-derived offset, oldest byte first.

    This is a best-effort log snapshot, not a checksummed crash record. A reset
    or a concurrent producer may interrupt a byte update. Do not infer that an
    operation finished merely because its entry message is present.
    """
    if len(data) != RETAINED_SIZE:
        raise ValueError(f"need {RETAINED_SIZE} bytes, got {len(data)}")
    magic, head, total = struct.unpack_from("<3I", data)
    if magic != RETAINED_LOG_MAGIC or head >= LOG_CAPACITY:
        raise ValueError(f"invalid retained ring magic=0x{magic:08x} head={head}")
    available = min(total, LOG_CAPACITY)
    ring = data[RETAINED_HEADER_SIZE:]
    start = (head - available) % LOG_CAPACITY
    log = (ring[start:] + ring[:start])[:available]
    return log, head, total


def checksum(data: bytes) -> int:
    value = 0x811C9DC5
    for byte in data:
        value ^= byte
        value = (value * 0x01000193) & 0xFFFFFFFF
    return value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", help="raw H2CORE record/partition or complete flash image")
    parser.add_argument("--offset", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--show-log", action="store_true", help="print retained log bytes")
    parser.add_argument(
        "--retained-ring", action="store_true",
        help="decode retained_log RAM instead of H2CORE; derive --offset from the flashed ELF",
    )
    args = parser.parse_args()
    with open(args.image, "rb") as source:
        source.seek(args.offset)
        data = source.read(RETAINED_SIZE if args.retained_ring else RECORD_SIZE)
    if args.retained_ring:
        try:
            log, head, total = decode_retained_log(data)
        except ValueError as error:
            print(f"invalid: {error}", file=sys.stderr)
            return 2
        print(f"retained_ring=true head={head} log_total={total} log_bytes={len(log)}")
        if args.show_log:
            print("H2_JIELI_RETAINED_LOG_BEGIN")
            print(log.decode("utf-8", errors="replace"), end="")
            if log and not log.endswith(b"\n"):
                print()
            print("H2_JIELI_RETAINED_LOG_END")
        return 0
    if len(data) != RECORD_SIZE:
        print(f"invalid: need {RECORD_SIZE} bytes, got {len(data)}", file=sys.stderr)
        return 2
    (
        size,
        magic,
        version,
        sequence,
        reset_reason,
        stage,
        caller,
        result,
        log_bytes,
        log_total,
    ) = struct.unpack_from(HEADER_FORMAT, data)
    checksum_offset = HEADER_SIZE + LOG_CAPACITY
    got_sum, committed = struct.unpack_from("<2I", data, checksum_offset)
    expected_sum = checksum(data[:checksum_offset])
    valid = (
        size == RECORD_SIZE
        and magic == MAGIC
        and version == VERSION
        and log_bytes <= LOG_CAPACITY
        and committed == COMMITTED
        and got_sum == expected_sum
    )
    print(
        f"valid={str(valid).lower()} magic=0x{magic:08x} version={version} "
        f"size={size} sequence={sequence} reset_reason=0x{reset_reason:08x} "
        f"boot_stage={stage} "
        f"caller=0x{caller:08x} marker_result={result} "
        f"log_bytes={log_bytes} log_total={log_total} "
        f"checksum=0x{got_sum:08x} expected=0x{expected_sum:08x} "
        f"committed=0x{committed:08x}"
    )
    if args.show_log and log_bytes <= LOG_CAPACITY:
        log = data[HEADER_SIZE : HEADER_SIZE + log_bytes]
        print("H2_JIELI_COREDUMP_LOG_BEGIN")
        print(log.decode("utf-8", errors="replace"), end="")
        if log and not log.endswith(b"\n"):
            print()
        print("H2_JIELI_COREDUMP_LOG_END")
    return 0 if valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
