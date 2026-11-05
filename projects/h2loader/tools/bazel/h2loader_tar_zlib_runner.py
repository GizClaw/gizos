#!/usr/bin/env python3
"""Create one H2Loader update archive and its release metadata."""

from __future__ import annotations

import argparse
import copy
from pathlib import Path
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[4]))
from projects.h2loader.tools.bazel.firmware_output import (
    encrypt_bk_images,
    publish_bk_recovery,
    publish_encryption_receipt,
    publish_esp_recovery,
    publish_managed_package,
    publish_metadata,
)


def native_artifact(value: str) -> tuple[str, Path]:
    name, separator, path = value.partition("=")
    if not separator or not name or not path:
        raise argparse.ArgumentTypeError("native artifact must be NAME=PATH")
    return name, Path(path)


_ENCRYPTION_FIELDS = (
    "flash_aes_key", "partition_metadata", "encrypted_image_output",
    "encrypted_recovery_output", "encryption_receipt",
)
_OUTPUT_FIELDS = (
    "package_output", "metadata_output", "factory_output", "recovery",
    "encrypted_image_output", "encrypted_recovery_output", "encryption_receipt",
)


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--app-image", required=True)
    parser.add_argument("--app-path", required=True)
    parser.add_argument("--entry", required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--board", required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--role", choices=("app", "h2loader"), required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--package-output", required=True)
    parser.add_argument("--package-format", type=int, choices=(1, 2), default=2)
    parser.add_argument("--metadata-output", required=True)
    parser.add_argument("--factory-image")
    parser.add_argument("--factory-output")
    parser.add_argument("--recovery")
    parser.add_argument("--esp-flash-root")
    parser.add_argument("--esp-flash-metadata")
    parser.add_argument("--bk-recovery-image")
    parser.add_argument("--bk-recovery-config")
    parser.add_argument("--flash-aes-key")
    parser.add_argument("--partition-metadata")
    parser.add_argument("--encrypted-image-output")
    parser.add_argument("--encrypted-recovery-output")
    parser.add_argument("--encryption-receipt")
    parser.add_argument("--package-data-root", default="")
    parser.add_argument("--package-data-file", action="append", default=[])
    # One per --package-data-file, in the same order: where its bytes are.
    parser.add_argument("--package-data-source", action="append", default=[])
    parser.add_argument(
        "--native-artifact",
        action="append",
        default=[],
        type=native_artifact,
    )
    repeatable = {"--package-data-file", "--package-data-source", "--native-artifact"}
    seen = set()
    for token in argv:
        flag = token.split("=", 1)[0]
        if not flag.startswith("--") or flag in repeatable:
            continue
        if flag in seen:
            parser.error("duplicate option: " + flag)
        seen.add(flag)
    return parser.parse_args(argv)


def _publish(arguments: argparse.Namespace, recovery_config: dict | None = None) -> None:
    source_root = Path(arguments.source_root).resolve()
    app_image = Path(arguments.app_image)
    bk_recovery_image = Path(arguments.bk_recovery_image) if arguments.bk_recovery_image else None
    package = Path(arguments.package_output)
    if bool(arguments.factory_image) != bool(arguments.factory_output):
        raise ValueError("factory image and output must be declared together")
    factory = Path(arguments.factory_output) if arguments.factory_output else None
    factory_required = arguments.platform == "esp" and arguments.role == "h2loader"
    if (factory is not None) != factory_required:
        raise ValueError("ESP H2Loader firmware must declare one factory image")
    recovery = Path(arguments.recovery) if arguments.recovery else None
    if factory is not None:
        factory.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(Path(arguments.factory_image), factory)
    if recovery and (arguments.esp_flash_root or bk_recovery_image):
        if arguments.platform == "esp":
            if not arguments.esp_flash_root or not arguments.esp_flash_metadata:
                raise ValueError("ESP Loader recovery requires flash files and metadata")
            publish_esp_recovery(
                flash_root=Path(arguments.esp_flash_root),
                flash_metadata=Path(arguments.esp_flash_metadata),
                output=recovery,
                board=arguments.board,
                target=arguments.target,
            )
        elif arguments.platform == "bk7258":
            if not bk_recovery_image or not arguments.bk_recovery_config:
                raise ValueError("BK Loader recovery requires image and layout config")
            publish_bk_recovery(
                recovery_image=bk_recovery_image,
                recovery_config=recovery_config if recovery_config is not None else Path(arguments.bk_recovery_config),
                output=recovery,
                board=arguments.board,
                target=arguments.target,
            )
    publish_managed_package(
        source_root=source_root,
        app_image=app_image,
        app_path=arguments.app_path,
        data_root=arguments.package_data_root,
        data_files=arguments.package_data_file,
        data_sources=arguments.package_data_source or arguments.package_data_file,
        output=package,
        board=arguments.board,
        role=arguments.role,
        target=arguments.target,
        version=arguments.version,
        package_format=arguments.package_format,
    )
    publish_metadata(
        output=Path(arguments.metadata_output),
        entry=arguments.entry,
        platform=arguments.platform,
        board=arguments.board,
        image=arguments.image,
        role=arguments.role,
        target=arguments.target,
        version=arguments.version,
        app_image=app_image,
        package=package,
        factory=factory,
        recovery=recovery,
        native=arguments.native_artifact,
        package_format=arguments.package_format,
    )


def _run_encrypted(arguments: argparse.Namespace) -> None:
    outputs = {field: Path(getattr(arguments, field)) for field in _OUTPUT_FIELDS if getattr(arguments, field)}
    resolved = [path.resolve() for path in outputs.values()]
    inputs = [Path(value).resolve() for value in (
        arguments.app_image, arguments.bk_recovery_image, arguments.bk_recovery_config,
        arguments.flash_aes_key, arguments.factory_image,
    ) if value]
    inputs += [(Path(arguments.source_root) / value).resolve() for value in (
        arguments.package_data_source or arguments.package_data_file
    )]
    metadata_root = Path(arguments.partition_metadata).resolve() if arguments.partition_metadata else None
    # Validate destinations before cleanup can remove anything. The rule always
    # declares distinct outputs; direct callers must not overwrite native inputs.
    if len(set(resolved)) != len(resolved) or any(
        path in inputs or (metadata_root is not None and path.is_relative_to(metadata_root))
        for path in resolved
    ) or any(path.is_symlink() or path.is_dir() for path in outputs.values()):
        raise ValueError("encrypted outputs must be distinct from each other and their inputs")
    try:
        if any(not getattr(arguments, field) for field in (
            "flash_aes_key", "partition_metadata", "encrypted_image_output", "encryption_receipt",
        )):
            raise ValueError("Flash AES requires a key file, partition metadata, image output and receipt")
        if arguments.platform != "bk7258" or arguments.target != "bk7258" or arguments.app_path != "app/bk/app_ab_crc.rbl":
            raise ValueError("Flash AES packages require BK7258 native inputs")
        if arguments.factory_image or arguments.factory_output or arguments.esp_flash_root or arguments.esp_flash_metadata:
            raise ValueError("Flash AES packages cannot contain ESP inputs")
        loader = arguments.role == "h2loader"
        if any(bool(getattr(arguments, field)) != loader for field in (
            "recovery", "bk_recovery_image", "bk_recovery_config", "encrypted_recovery_output",
        )):
            raise ValueError("Flash AES Loader requires encrypted recovery; Apps cannot declare recovery")
        if loader and (arguments.package_data_file or arguments.package_data_source):
            raise ValueError("h2loader firmware cannot declare package data")
        package = Path(arguments.package_output)
        package.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".h2loader-encrypted-", dir=package.parent) as temporary:
            staged = copy.copy(arguments)
            replacements = {}
            for index, (field, destination) in enumerate(outputs.items()):
                path = Path(temporary) / str(index) / destination.name
                path.parent.mkdir()
                setattr(staged, field, str(path))
                replacements[destination] = path
            staged.native_artifact = [(name, replacements.get(path, path)) for name, path in arguments.native_artifact]
            encryption = encrypt_bk_images(
                key_file=Path(arguments.flash_aes_key),
                partition_metadata=Path(arguments.partition_metadata),
                app_image=Path(arguments.app_image),
                app_output=Path(staged.encrypted_image_output),
                recovery_image=Path(arguments.bk_recovery_image) if loader else None,
                recovery_config=Path(arguments.bk_recovery_config) if loader else None,
                recovery_output=Path(staged.encrypted_recovery_output) if loader else None,
            )
            staged.app_image = staged.encrypted_image_output
            staged.bk_recovery_image = staged.encrypted_recovery_output
            _publish(staged, encryption.recovery_config)
            publish_encryption_receipt(
                output=Path(staged.encryption_receipt),
                board=arguments.board,
                role=arguments.role,
                target=arguments.target,
                version=arguments.version,
                encryption=encryption,
                encrypted_image=Path(staged.encrypted_image_output),
                package=Path(staged.package_output),
                recovery=Path(staged.recovery) if loader else None,
            )
            for field, destination in outputs.items():
                destination.parent.mkdir(parents=True, exist_ok=True)
                Path(getattr(staged, field)).replace(destination)
    except BaseException:
        for path in outputs.values():
            path.unlink(missing_ok=True)
        raise


def run(arguments: argparse.Namespace) -> None:
    if any(getattr(arguments, field) is not None for field in _ENCRYPTION_FIELDS):
        _run_encrypted(arguments)
    else:
        _publish(arguments)


def main(argv: list[str]) -> int:
    encrypted = any(token.split("=", 1)[0] == "--" + field.replace("_", "-") for token in argv for field in _ENCRYPTION_FIELDS)
    try:
        run(parse_arguments(argv))
    except (OSError, ValueError, KeyError, TypeError, struct.error) as error:
        # Cipher/input exceptions may carry untrusted contents. Keep keyed
        # failures value-free; plaintext packaging retains its usual diagnostics.
        print("error: encrypted package refused: invalid input or publication failed" if encrypted else f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
