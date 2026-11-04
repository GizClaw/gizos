"""Encrypt native BK7258 images independently of any firmware package format."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile

from tools.bazel.bk7258_flash_aes import (
    encrypt_app,
    encrypt_recovery,
    validate_app,
    validate_recovery,
)
from tools.bazel.firmware_crypto import message, protect_process, read_key


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--source", required=True)
    parser.add_argument("--metadata", required=True, help="Native partition-metadata directory")
    parser.add_argument("--image", required=True, choices=("app", "full"))
    parser.add_argument("--key-fd", type=int, required=True, help="Pipe/socket carrying exactly 32 raw bytes and EOF")
    parser.add_argument("--output-dir", required=True, help="New absolute directory outside Bazel outputs")
    tokens = sys.argv[1:] if argv is None else argv
    for flag in ("--source", "--metadata", "--image", "--key-fd", "--output-dir"):
        if sum(token == flag or token.startswith(flag + "=") for token in tokens) > 1:
            parser.error("duplicate option")
    args = parser.parse_args(tokens)
    temporary = None
    key = bytearray()
    try:
        protect_process()
        output = Path(args.output_dir)
        if not output.is_absolute() or output.exists() or output.is_symlink():
            raise ValueError("OUTPUT_DIRECTORY")
        parent = output.parent.resolve(strict=True)
        if any(part in ("bazel-out", "execroot") or part.endswith(".runfiles") for part in parent.parts):
            raise ValueError("BAZEL_OUTPUT_DIRECTORY")
        with Path(args.source).open("rb") as stream:
            source = stream.read(16 * 1024 * 1024 + 1)
        if not 0 < len(source) <= 16 * 1024 * 1024:
            raise ValueError("SOURCE_LENGTH")
        with (Path(args.metadata) / "partitions.json").open("rb") as stream:
            metadata_bytes = stream.read(1024 * 1024 + 1)
        if len(metadata_bytes) > 1024 * 1024:
            raise ValueError("METADATA_SIZE")
        metadata = json.loads(metadata_bytes)
        if not isinstance(metadata, dict):
            raise ValueError("METADATA_FORMAT")
        if args.image == "app":
            _, logical_base, _ = validate_app(source, metadata)
            transform = encrypt_app
            filename = "app.flash-aes.rbl"
        else:
            validate_recovery(source, metadata)
            logical_base = 0
            transform = encrypt_recovery
            filename = "full.flash-aes.bin"
        # Snapshot every unkeyed input before accepting the runtime descriptor.
        key = read_key(args.key_fd)
        encrypted = transform(source, metadata, bytes(key))
        temporary = Path(tempfile.mkdtemp(prefix=".bk-firmware-", dir=parent))
        receipt = {
            "format": "bk7258-encrypted-firmware-v1",
            "target": "bk7258",
            "image": args.image,
            "logical_base": logical_base,
            "physical_base": logical_base // 32 * 34,
            "source_image_sha256": hashlib.sha256(source).hexdigest(),
            "image_sha256": hashlib.sha256(encrypted).hexdigest(),
            "image_size": len(encrypted),
            "partition_metadata_sha256": hashlib.sha256(metadata_bytes).hexdigest(),
        }
        for name, data in (
            (filename, encrypted),
            ("validation.json", (json.dumps(receipt, indent=2) + "\n").encode()),
        ):
            with (temporary / name).open("xb") as stream:
                os.fchmod(stream.fileno(), 0o600)
                stream.write(data)
        if output.exists() or output.is_symlink():
            raise ValueError("OUTPUT_DIRECTORY")
        temporary.rename(output)
        temporary = None
        message("Encrypted BK7258 native image created.")
        return 0
    except (OSError, ValueError, KeyError, TypeError, struct.error, UnicodeError):
        message("BK7258 encryption refused: invalid native image, layout, descriptor or output.", error=True)
        return 1
    finally:
        key[:] = b"\0" * len(key)
        if temporary is not None:
            try:
                shutil.rmtree(temporary)
            except OSError:
                message("Private firmware staging cleanup failed.", error=True)


if __name__ == "__main__":
    raise SystemExit(main())
