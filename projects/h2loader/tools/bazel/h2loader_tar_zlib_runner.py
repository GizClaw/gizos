#!/usr/bin/env python3
"""Create one H2Loader update archive and its release metadata."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[4]))
from projects.h2loader.tools.bazel.firmware_output import (
    publish_bk_recovery,
    publish_esp_recovery,
    publish_managed_package,
    publish_metadata,
)


def native_artifact(value: str) -> tuple[str, Path]:
    name, separator, path = value.partition("=")
    if not separator or not name or not path:
        raise argparse.ArgumentTypeError("native artifact must be NAME=PATH")
    return name, Path(path)


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
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
    parser.add_argument("--metadata-output", required=True)
    parser.add_argument("--recovery")
    parser.add_argument("--esp-flash-root")
    parser.add_argument("--esp-flash-metadata")
    parser.add_argument("--bk-recovery-image")
    parser.add_argument("--bk-recovery-config")
    parser.add_argument("--package-data-root", default="")
    parser.add_argument("--package-data-file", action="append", default=[])
    parser.add_argument(
        "--native-artifact",
        action="append",
        default=[],
        type=native_artifact,
    )
    return parser.parse_args(argv)


def run(arguments: argparse.Namespace) -> None:
    source_root = Path(arguments.source_root).resolve()
    app_image = Path(arguments.app_image)
    package = Path(arguments.package_output)
    recovery = Path(arguments.recovery) if arguments.recovery else None
    if recovery and (arguments.esp_flash_root or arguments.bk_recovery_image):
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
            if not arguments.bk_recovery_image or not arguments.bk_recovery_config:
                raise ValueError("BK Loader recovery requires image and layout config")
            publish_bk_recovery(
                recovery_image=Path(arguments.bk_recovery_image),
                recovery_config=Path(arguments.bk_recovery_config),
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
        output=package,
        board=arguments.board,
        role=arguments.role,
        target=arguments.target,
        version=arguments.version,
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
        recovery=recovery,
        native=arguments.native_artifact,
    )


def main(argv: list[str]) -> int:
    try:
        run(parse_arguments(argv))
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
