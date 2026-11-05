"""Package an already transformed native image without handling encryption keys."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile

from projects.h2loader.tools.bazel.firmware_artifacts import read_managed_package, write_package
from projects.h2loader.tools.bazel.firmware_output import publish_bk_recovery, publish_metadata, sha256
from tools.bazel.firmware_crypto import message as _message


def _read(path, limit):
    with Path(path).open("rb") as stream:
        data = stream.read(limit + 1)
    if not data or len(data) > limit:
        raise ValueError("INPUT_SIZE")
    return data


def _write(path, data):
    with path.open("xb") as stream:
        os.fchmod(stream.fileno(), 0o600)
        stream.write(data)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    commands = parser.add_subparsers(dest="mode", required=True)
    app = commands.add_parser("app", allow_abbrev=False)
    app.add_argument("--source", required=True, help="Original managed App package")
    recovery = commands.add_parser("recovery", allow_abbrev=False)
    recovery.add_argument("--config", required=True, help="Board-owned recovery flashing configuration")
    recovery.add_argument("--board", required=True)
    recovery.add_argument("--target", required=True, choices=("bk7258",))
    for command in (app, recovery):
        command.add_argument("--image", required=True, help="Processed native image")
        command.add_argument("--receipt", required=True, help="Native transform's validation.json")
        command.add_argument("--output-dir", required=True)
    tokens = sys.argv[1:] if argv is None else argv
    for flag in ("--source", "--config", "--board", "--target", "--image", "--receipt", "--output-dir"):
        if sum(token == flag or token.startswith(flag + "=") for token in tokens) > 1:
            parser.error("duplicate option")
    args = parser.parse_args(tokens)
    temporary = None
    try:
        output = Path(args.output_dir)
        if not output.is_absolute() or output.exists() or output.is_symlink():
            raise ValueError("OUTPUT_DIRECTORY")
        parent = output.parent.resolve(strict=True)
        if any(part in ("bazel-out", "execroot") or part.endswith(".runfiles") for part in parent.parts):
            raise ValueError("BAZEL_OUTPUT_DIRECTORY")
        image_bytes = _read(args.image, 16 * 1024 * 1024)
        receipt = json.loads(_read(args.receipt, 1024 * 1024))
        if not isinstance(receipt, dict) or receipt.get("image_sha256") != hashlib.sha256(image_bytes).hexdigest():
            raise ValueError("IMAGE_RECEIPT")
        if type(receipt.get("image_size")) is not int or receipt["image_size"] != len(image_bytes):
            raise ValueError("IMAGE_SIZE")
        if args.mode == "app":
            source = _read(args.source, 128 * 1024 * 1024)
            package = read_managed_package(source)
            identity = package.manifest
            if (identity["role"] != "app" or receipt.get("image") != "app"
                    or receipt.get("target") != identity["target"]
                    or receipt.get("source_image_sha256") != identity["image_sha256"]):
                raise ValueError("APP_RECEIPT")
        else:
            if (receipt.get("image") != "full" or receipt.get("target") != args.target
                    or type(receipt.get("physical_base")) is not int or receipt["physical_base"] != 0):
                raise ValueError("RECOVERY_RECEIPT")
            settings = json.loads(_read(args.config, 1024 * 1024))
            if (not isinstance(settings, dict) or not isinstance(settings.get("offset"), str)
                    or int(settings["offset"], 0) != 0 or type(settings.get("baud")) is not int
                    or settings["baud"] <= 0):
                raise ValueError("RECOVERY_CONFIG")
        temporary = Path(tempfile.mkdtemp(prefix=".h2-package-", dir=parent))
        image = temporary / "image.bin"
        _write(image, image_bytes)
        validation = {
            "format": "h2loader-repackaged-image-v1",
            "image_sha256": receipt["image_sha256"],
            "native_validation": {
                field: receipt[field] for field in (
                    "format", "target", "image", "logical_base", "physical_base",
                    "source_image_sha256", "image_sha256", "image_size", "partition_metadata_sha256",
                ) if field in receipt
            },
        }
        if args.mode == "app":
            fmt = int(identity["format"])
            archive = temporary / ("app.update.tar.zlib" if fmt == 1 else "app.update.tar")
            write_package(archive, package.app_path or "app/image.bin", image_bytes, package.data,
                          board=identity["board"], target=identity["target"], role="app",
                          version=identity["version"], package_format=fmt)
            publish_metadata(output=temporary / "firmware.json", entry="offline/repackage",
                             platform=identity["target"], board=identity["board"], image="app", role="app",
                             target=identity["target"], version=identity["version"], app_image=image,
                             package=archive, factory=None, recovery=None, native=[], package_format=fmt)
            validation.update(source_package_sha256=hashlib.sha256(source).hexdigest(), package_sha256=sha256(archive))
        else:
            config = temporary / ".recovery-config.json"
            _write(config, json.dumps(settings).encode())
            archive = temporary / "recovery.h2fb"
            publish_bk_recovery(recovery_image=image, recovery_config=config, output=archive,
                                board=args.board, target=args.target)
            config.unlink()
            validation.update(board=args.board, target=args.target, bundle_sha256=sha256(archive))
        _write(temporary / "validation.json", (json.dumps(validation, indent=2) + "\n").encode())
        for path in temporary.iterdir():
            os.chmod(path, 0o600)
        if output.exists() or output.is_symlink():
            raise ValueError("OUTPUT_DIRECTORY")
        temporary.rename(output)
        temporary = None
        _message("H2Loader package created from the processed native image.")
        return 0
    except (OSError, ValueError, KeyError, TypeError, struct.error, UnicodeError):
        _message("H2Loader packaging refused: invalid image, receipt, source package or output.", error=True)
        return 1
    finally:
        if temporary is not None:
            try:
                shutil.rmtree(temporary)
            except OSError:
                _message("Private package staging cleanup failed.", error=True)


if __name__ == "__main__":
    raise SystemExit(main())
