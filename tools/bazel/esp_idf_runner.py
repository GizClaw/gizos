#!/usr/bin/env python3
"""Run one local ESP-IDF build and publish validated native artifacts."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.bazel.native_ccache import (
    NativeCcacheError,
    configure_environment as configure_ccache_environment,
)
from tools.bazel.native_runtime import (
    NativeRuntimeError,
    find_external_repository_source_root,
    find_unique_executable,
    fixed_environment,
    locator_path,
    read_locator,
)

H2LOADER_WIFI_CREDENTIALS = "H2LOADER_WIFI_CREDENTIALS"
NATIVE_BUILD_JOBS = "H2_NATIVE_BUILD_JOBS"
GIZOS_ROOT = Path(__file__).resolve().parents[2]
EXPECTED_IDF_VERSION = "6.0"
TARGET_COMPILERS = {
    "esp32c5": "riscv32-esp-elf-gcc",
    "esp32p4": "riscv32-esp-elf-gcc",
    "esp32s3": "xtensa-esp-elf-gcc",
}
SAFE_PROJECT_NAME = re.compile(r"^[A-Za-z0-9_]+$")
SAFE_COMPONENT_NAME = re.compile(r"^[a-z][a-z0-9_]*$")
SAFE_CMAKE_VARIABLE_NAME = re.compile(r"^[A-Z][A-Z0-9_]*$")
COMMIT = re.compile(r"^[0-9a-f]{40}$")
REQUIRED_TOOL_VERSION_KEYS = frozenset((*TARGET_COMPILERS, "ninja", "python"))
TOOL_VERSION_KEYS = REQUIRED_TOOL_VERSION_KEYS | {
    "esp32s3_archive_compiler",
    "esp32s3_dynconfig",
}
LAUNCHER_TARGET = re.compile(
    r'^\s*set\(\s*H2_ESP_TARGET\s+"([^"]+)"',
    re.MULTILINE,
)


class RunnerError(RuntimeError):
    """A violated external-build contract."""


def read_expected_commit(path: str, label: str) -> str:
    try:
        commit = Path(path).read_text(encoding="utf-8").strip()
    except OSError as error:
        raise RunnerError(f"cannot read {label} version file: {path}: {error}") from error
    if not COMMIT.fullmatch(commit):
        raise RunnerError(f"invalid expected {label} commit: {commit}")
    return commit


def read_expected_tool_versions(path: str) -> dict[str, str]:
    try:
        lines = Path(path).read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise RunnerError(
            f"cannot read ESP-IDF tool versions file: {path}: {error}"
        ) from error
    versions: dict[str, str] = {}
    for line in lines:
        key, separator, value = line.partition("=")
        if (
            not separator
            or key not in TOOL_VERSION_KEYS
            or not value
            or key in versions
        ):
            raise RunnerError(f"invalid ESP-IDF tool version entry: {line}")
        versions[key] = value
    if not REQUIRED_TOOL_VERSION_KEYS.issubset(versions):
        missing = ", ".join(sorted(REQUIRED_TOOL_VERSION_KEYS - versions.keys()))
        raise RunnerError(f"ESP-IDF tool versions are incomplete: {missing}")
    return versions


def require_environment(arguments: argparse.Namespace) -> dict[str, str]:
    sdk_locator = read_locator(arguments.idf_sdk_locator, "esp-idf-sdk")
    tools_locator = read_locator(arguments.idf_tools_locator, "esp-idf-tools")
    idf_path = locator_path(sdk_locator, "root", "esp-idf-sdk")
    python_root = locator_path(tools_locator, "python_root", "esp-idf-tools")
    tools_root = locator_path(tools_locator, "tools_root", "esp-idf-tools")
    compiler_name = TARGET_COMPILERS.get(arguments.target)
    if not compiler_name:
        raise RunnerError(f"unsupported ESP target: {arguments.target}")
    compiler = find_unique_executable(tools_root, compiler_name, compiler_name)
    environment = fixed_environment([
        idf_path / "tools",
        python_root / "bin",
        compiler.parent,
    ])
    ninja = shutil.which("ninja", path=environment["PATH"])
    if ninja is None:
        ninja = str(find_unique_executable(tools_root, "ninja", "Ninja"))
        environment["PATH"] = f"{Path(ninja).parent}{os.pathsep}{environment['PATH']}"
    environment.update({
        "IDF_PATH": str(idf_path),
        "IDF_PYTHON_ENV_PATH": str(python_root),
        "IDF_TOOLS_PATH": str(tools_root),
    })
    if arguments.h2loader_wifi_environment:
        credentials = os.environ.get(H2LOADER_WIFI_CREDENTIALS)
        if credentials is None:
            raise RunnerError(
                f"required action environment is missing: {H2LOADER_WIFI_CREDENTIALS}"
            )
        environment.update(parse_h2loader_wifi_credentials(credentials))
    environment[NATIVE_BUILD_JOBS] = "4"
    return environment


def parse_h2loader_wifi_credentials(value: str) -> dict[str, str]:
    try:
        credentials = json.loads(value)
    except json.JSONDecodeError as error:
        raise RunnerError(
            f"{H2LOADER_WIFI_CREDENTIALS} must be valid JSON"
        ) from error
    if not isinstance(credentials, dict) or set(credentials) != {
        "ssid",
        "password",
    }:
        raise RunnerError(
            f"{H2LOADER_WIFI_CREDENTIALS} must contain exactly ssid and password"
        )
    for name in ("ssid", "password"):
        credential = credentials[name]
        if not isinstance(credential, str) or not credential:
            raise RunnerError(
                f"{H2LOADER_WIFI_CREDENTIALS}.{name} must be a non-empty string"
            )
        if any(character in credential for character in "\r\n;"):
            raise RunnerError(
                f"{H2LOADER_WIFI_CREDENTIALS}.{name} contains an unsupported character"
            )
    return {
        "H2LOADER_WIFI_SSID": credentials["ssid"],
        "H2LOADER_WIFI_PASSWORD": credentials["password"],
    }


def require_executable(path: Path, label: str) -> Path:
    if not path.is_file() or not os.access(path, os.X_OK):
        raise RunnerError(f"{label} is unavailable or not executable: {path}")
    return path


def validate_tools(
    environment: dict[str, str],
    target: str,
) -> tuple[Path, Path, Path, Path, Path]:
    idf_path = Path(environment["IDF_PATH"]).resolve()
    python_environment = Path(environment["IDF_PYTHON_ENV_PATH"]).resolve()
    tools_path = Path(environment["IDF_TOOLS_PATH"]).resolve()
    if not tools_path.is_dir():
        raise RunnerError(f"IDF_TOOLS_PATH is not a directory: {tools_path}")
    idf_py = require_executable(idf_path / "tools" / "idf.py", "idf.py")
    python_executable = require_executable(
        python_environment / "bin" / "python",
        "IDF Python",
    )
    compiler = TARGET_COMPILERS.get(target)
    if compiler is None:
        raise RunnerError(f"unsupported ESP target: {target}")
    compiler_path = shutil.which(compiler, path=environment["PATH"])
    if compiler_path is None:
        raise RunnerError(f"required {target} compiler is unavailable in PATH: {compiler}")
    compiler_executable = require_executable(
        Path(compiler_path).resolve(),
        f"{target} compiler",
    )
    ninja_path = shutil.which("ninja", path=environment["PATH"])
    if ninja_path is None:
        raise RunnerError("required ESP-IDF build backend is unavailable in PATH: ninja")
    ninja = require_executable(Path(ninja_path).resolve(), "Ninja")
    return idf_path, idf_py, python_executable, compiler_executable, ninja


def command_output(
    command: list[str],
    environment: dict[str, str],
    label: str,
) -> str:
    try:
        result = subprocess.run(
            command,
            check=False,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except OSError as error:
        raise RunnerError(f"cannot inspect {label}: {error}") from error
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "no output"
        raise RunnerError(f"{label} validation failed: {detail}")
    return result.stdout.strip()


def validate_tool_versions(
    idf_path: Path,
    idf_python: Path,
    compiler: Path,
    ninja: Path,
    target: str,
    expected: dict[str, str],
    environment: dict[str, str],
) -> None:
    compiler_version = command_output(
        [str(compiler), "--version"],
        environment,
        f"{target} compiler",
    ).splitlines()[0]
    if compiler_version != expected[target]:
        raise RunnerError(
            f"{target} compiler version mismatch: expected {expected[target]}, "
            f"found {compiler_version}"
        )
    ninja_version = command_output(
        [str(ninja), "--version"],
        environment,
        "Ninja",
    )
    if ninja_version != expected["ninja"]:
        raise RunnerError(
            f"Ninja version mismatch: expected {expected['ninja']}, "
            f"found {ninja_version}"
        )
    if expected["python"] != "esp-idf-v6.0-constraints":
        raise RunnerError(f"unsupported ESP-IDF Python identity: {expected['python']}")
    idf_tools = idf_path / "tools" / "idf_tools.py"
    for operation in ("check", "check-python-dependencies"):
        command_output(
            [str(idf_python), str(idf_tools), "--non-interactive", operation],
            environment,
            f"ESP-IDF tools {operation}",
        )


def create_ninja_wrapper(
    temporary_root: Path,
    ninja: Path,
    jobs: str,
) -> Path:
    wrapper_directory = temporary_root / "build-tools"
    wrapper_directory.mkdir()
    wrapper = wrapper_directory / "ninja"
    wrapper.write_text(
        "#!/bin/sh\n"
        f"exec {shlex.quote(str(ninja))} -j {shlex.quote(jobs)} \"$@\"\n",
        encoding="utf-8",
    )
    wrapper.chmod(0o755)
    return wrapper_directory


def validate_commit(
    idf_path: Path,
    expected_commit: str,
    environment: dict[str, str],
) -> None:
    if not COMMIT.fullmatch(expected_commit):
        raise RunnerError(f"invalid expected ESP-IDF commit: {expected_commit}")
    try:
        result = subprocess.run(
            ["git", "-C", str(idf_path), "rev-parse", "HEAD"],
            check=False,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except OSError as error:
        raise RunnerError(f"cannot inspect ESP-IDF checkout: {error}") from error
    actual_commit = result.stdout.strip() if result.returncode == 0 else "unknown"
    if actual_commit != expected_commit:
        raise RunnerError(
            "ESP-IDF commit mismatch: "
            f"expected {expected_commit}, found {actual_commit}"
        )


def resolve_project(source_root: Path, project_file: Path, target: str) -> Path:
    if project_file.is_absolute() or ".." in project_file.parts:
        raise RunnerError(f"launcher project must be source-root relative: {project_file}")
    project_file = source_root / project_file
    if project_file.name != "CMakeLists.txt" or not project_file.is_file():
        raise RunnerError(f"launcher project CMakeLists.txt is missing: {project_file}")
    matches = LAUNCHER_TARGET.findall(project_file.read_text(encoding="utf-8"))
    if len(matches) != 1:
        raise RunnerError(
            f"expected one H2_ESP_TARGET in {project_file}, found {len(matches)}"
        )
    if matches[0] != target:
        raise RunnerError(
            f"launcher target mismatch: rule={target}, CMake={matches[0]}"
        )
    return project_file.parent


def required_file(path: Path, label: str) -> Path:
    if not path.is_file():
        raise RunnerError(f"required ESP-IDF output is missing: {label}: {path}")
    if path.stat().st_size == 0:
        raise RunnerError(f"required ESP-IDF output is empty: {label}: {path}")
    return path


def validate_flash_metadata(
    build_directory: Path,
) -> tuple[dict[str, object], list[tuple[str, Path]]]:
    metadata_path = required_file(
        build_directory / "flasher_args.json",
        "flash metadata",
    )
    try:
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise RunnerError(f"invalid ESP-IDF flash metadata: {error}") from error
    if not isinstance(metadata, dict):
        raise RunnerError("ESP-IDF flash metadata must be a JSON object")
    flash_files = metadata.get("flash_files")
    if not isinstance(flash_files, dict) or not flash_files:
        raise RunnerError("ESP-IDF flash metadata has no flash_files object")

    build_root = build_directory.resolve()
    seen_offsets: set[int] = set()
    normalized: dict[str, str] = {}
    sources: list[tuple[str, Path]] = []
    for offset, relative in flash_files.items():
        if not isinstance(offset, str) or not isinstance(relative, str) or not relative:
            raise RunnerError("ESP-IDF flash metadata contains a non-string offset or path")
        try:
            numeric_offset = int(offset, 0)
        except ValueError as error:
            raise RunnerError(f"invalid ESP-IDF flash offset: {offset}") from error
        if numeric_offset < 0 or numeric_offset > 0xFFFFFFFF:
            raise RunnerError(f"ESP-IDF flash offset is out of range: {offset}")
        if numeric_offset in seen_offsets:
            raise RunnerError(f"duplicate ESP-IDF flash offset: {offset}")
        seen_offsets.add(numeric_offset)

        relative_path = Path(relative)
        if relative_path.is_absolute():
            raise RunnerError(f"ESP-IDF flash path must be relative: {relative}")
        source = (build_root / relative_path).resolve()
        try:
            normalized_relative = source.relative_to(build_root)
        except ValueError as error:
            raise RunnerError(f"ESP-IDF flash path escapes build directory: {relative}") from error
        required_file(source, f"flash file at {offset}")
        normalized_path = normalized_relative.as_posix()
        normalized[offset] = normalized_path
        sources.append((normalized_path, source))

    normalized_metadata = dict(metadata)
    normalized_metadata["flash_files"] = normalized
    return normalized_metadata, sources


def copy_output(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


def copy_support_files(
    source_root: Path,
    temporary_source_root: Path,
    support_files: list[str],
) -> None:
    for value in support_files:
        relative = Path(value)
        if relative.is_absolute() or ".." in relative.parts:
            raise RunnerError(f"support file must be source-root relative: {value}")
        source = required_file(source_root / relative, "launcher support file")
        copy_output(source, temporary_source_root / relative)


def resolve_prebuilt_components(
    source_root: Path,
    values: list[str],
) -> dict[str, Path]:
    source_root = source_root.resolve()
    components: dict[str, Path] = {}
    for value in values:
        component_name, separator, archive_value = value.partition("=")
        if (
            not separator
            or not SAFE_COMPONENT_NAME.fullmatch(component_name)
            or component_name in components
        ):
            raise RunnerError(f"invalid ESP-IDF prebuilt component: {value}")
        relative_archive = Path(archive_value)
        if (
            not archive_value
            or relative_archive.is_absolute()
            or ".." in relative_archive.parts
            or relative_archive.suffix != ".a"
        ):
            raise RunnerError(f"invalid ESP-IDF prebuilt archive path: {archive_value}")
        archive = required_file(
            (source_root / relative_archive).resolve(),
            f"prebuilt component {component_name}",
        )
        try:
            archive.relative_to(source_root)
        except ValueError as error:
            raise RunnerError(
                f"ESP-IDF prebuilt archive escapes source root: {archive_value}"
            ) from error
        components[component_name] = archive
    return components


def resolve_prebuilt_component_includes(
    source_root: Path,
    values: list[str],
    components: dict[str, Path],
) -> dict[str, list[Path]]:
    includes = {name: [] for name in components}
    for value in values:
        component_name, separator, raw_path = value.partition("=")
        relative_path = Path(raw_path)
        if (
            not separator
            or component_name not in components
            or not raw_path
            or relative_path.is_absolute()
            or ".." in relative_path.parts
        ):
            raise RunnerError(f"invalid ESP-IDF prebuilt component include: {value}")
        include = source_root / relative_path
        if not include.is_dir():
            raise RunnerError(f"ESP-IDF prebuilt component include is not a directory: {raw_path}")
        if include not in includes[component_name]:
            includes[component_name].append(include)
    return includes


def render_prebuilt_component(
    component_name: str,
    includes: list[Path],
    archive_helper: Path,
) -> str:
    lines = ["idf_component_register("]
    if includes:
        lines.append("  INCLUDE_DIRS")
        lines.extend(f'    "{include.as_posix()}"' for include in includes)
    lines.extend([
        ")",
        "",
        "if(CMAKE_BUILD_EARLY_EXPANSION)",
        "  return()",
        "endif()",
        f'include("{archive_helper.as_posix()}")',
        f"h2_idf_import_bazel_archive({component_name} H2_BAZEL_PREBUILT_{component_name.upper()})",
        "",
    ])
    return "\n".join(lines)


def resolve_native_components(
    source_root: Path,
    values: list[str],
) -> dict[str, Path]:
    components: dict[str, Path] = {}
    for value in values:
        component_name, separator, directory_value = value.partition("=")
        relative_directory = Path(directory_value)
        if (
            not separator
            or not SAFE_COMPONENT_NAME.fullmatch(component_name)
            or component_name in components
            or not directory_value
            or relative_directory.is_absolute()
            or ".." in relative_directory.parts
        ):
            raise RunnerError(f"invalid ESP-IDF native component: {value}")
        directory = source_root / relative_directory
        if not directory.is_dir() or not (directory / "CMakeLists.txt").is_file():
            raise RunnerError(
                f"ESP-IDF native component is missing CMakeLists.txt: {directory_value}"
            )
        components[component_name] = directory
    return components


def resolve_native_component_sources(
    source_root: Path,
    values: list[str],
) -> dict[str, list[Path]]:
    components: dict[str, list[Path]] = {}
    owners: dict[Path, str] = {}
    for value in values:
        component_name, separator, source_value = value.partition("=")
        relative_source = Path(source_value)
        if (
            not separator
            or not SAFE_COMPONENT_NAME.fullmatch(component_name)
            or not source_value
            or relative_source.is_absolute()
            or ".." in relative_source.parts
        ):
            raise RunnerError(f"invalid ESP-IDF native component source: {value}")
        source = required_file(
            source_root / relative_source,
            f"native component {component_name} source",
        )
        previous_owner = owners.get(source)
        if previous_owner is not None:
            raise RunnerError(
                f"native source has multiple owners: {source}: "
                f"{previous_owner}, {component_name}"
            )
        owners[source] = component_name
        components.setdefault(component_name, []).append(source)
    return components


def render_native_component_manifest(
    board: str,
    components: dict[str, Path],
    sources: dict[str, list[Path]],
) -> str:
    undeclared = sorted(set(sources) - set(components))
    if undeclared:
        raise RunnerError(
            "native component sources have no declared component: "
            + ", ".join(undeclared)
        )
    lines = [
        "# Generated from the Bazel native component graph.",
        f'set(H2_BAZEL_BOARD "{board}")',
        "set(H2_BAZEL_COMPONENT_DIRS",
    ]
    for component_name in sorted(components):
        lines.append(f'  "{components[component_name].as_posix()}"')
    lines.extend([")", "set(H2_BAZEL_COMPONENT_NAMES"])
    for component_name in sorted(components):
        lines.append(f'  "{component_name}"')
    lines.append(")")
    for component_name in sorted(components):
        lines.append(f"set(H2_BAZEL_COMPONENT_SRCS_{component_name.upper()}")
        for source in sorted(sources.get(component_name, [])):
            lines.append(f'  "{source.as_posix()}"')
        lines.append(")")
    return "\n".join(lines) + "\n"


def publish_outputs(
    build_directory: Path,
    project_name: str,
    arguments: argparse.Namespace,
) -> None:
    native_outputs = {
        "application ELF": build_directory / f"{project_name}.elf",
        "application map": build_directory / f"{project_name}.map",
        "application image": build_directory / f"{project_name}.bin",
        "bootloader image": build_directory / "bootloader" / "bootloader.bin",
        "combined factory image": build_directory / "combined_factory.bin",
        "partition-table image": (
            build_directory / "partition_table" / "partition-table.bin"
        ),
    }
    for label, path in native_outputs.items():
        required_file(path, label)

    metadata, flash_sources = validate_flash_metadata(build_directory)
    referenced_sources = {source.resolve() for _, source in flash_sources}
    for label in ("application image", "bootloader image", "partition-table image"):
        if native_outputs[label].resolve() not in referenced_sources:
            raise RunnerError(f"ESP-IDF flash metadata does not reference {label}")

    flash_output = Path(arguments.flash_files_output)
    if flash_output.exists():
        if not flash_output.is_dir() or any(flash_output.iterdir()):
            raise RunnerError(f"flash-files output is not an empty directory: {flash_output}")
    else:
        flash_output.mkdir(parents=True)
    for relative, source in flash_sources:
        copy_output(source, flash_output / relative)

    copy_output(native_outputs["application ELF"], Path(arguments.elf_output))
    copy_output(native_outputs["application map"], Path(arguments.map_output))
    copy_output(native_outputs["application image"], Path(arguments.app_output))
    copy_output(native_outputs["bootloader image"], Path(arguments.bootloader_output))
    copy_output(
        native_outputs["combined factory image"],
        Path(arguments.combined_factory_output),
    )
    copy_output(
        native_outputs["partition-table image"],
        Path(arguments.partition_table_output),
    )
    metadata_output = Path(arguments.flash_metadata_output)
    metadata_output.parent.mkdir(parents=True, exist_ok=True)
    metadata_output.write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--project", required=True)
    parser.add_argument("--partition", required=True)
    parser.add_argument("--project-name", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--board", required=True)
    parser.add_argument("--cmake-variable", action="append", default=[])
    parser.add_argument("--version", required=True)
    parser.add_argument("--idf-version-file", required=True)
    parser.add_argument("--idf-tool-versions-file", required=True)
    parser.add_argument("--idf-sdk-locator", required=True)
    parser.add_argument("--idf-tools-locator", required=True)
    parser.add_argument("--ccache-runtime-locator", required=True)
    parser.add_argument("--h2loader-wifi-environment", action="store_true")
    parser.add_argument("--elf-output", required=True)
    parser.add_argument("--map-output", required=True)
    parser.add_argument("--app-output", required=True)
    parser.add_argument("--bootloader-output", required=True)
    parser.add_argument("--combined-factory-output", required=True)
    parser.add_argument("--partition-table-output", required=True)
    parser.add_argument("--flash-files-output", required=True)
    parser.add_argument("--flash-metadata-output", required=True)
    parser.add_argument("--support-file", action="append", default=[])
    parser.add_argument("--project-support-file", action="append", default=[])
    parser.add_argument("--prebuilt-component", action="append", default=[])
    parser.add_argument("--prebuilt-component-include", action="append", default=[])
    parser.add_argument("--generate-prebuilt-component", action="append", default=[])
    parser.add_argument("--native-component", action="append", default=[])
    parser.add_argument("--native-component-source", action="append", default=[])
    return parser.parse_args(argv)


def run(arguments: argparse.Namespace) -> None:
    if not SAFE_PROJECT_NAME.fullmatch(arguments.project_name):
        raise RunnerError(f"invalid ESP-IDF project name: {arguments.project_name}")
    cmake_variables: list[tuple[str, str]] = []
    for entry in arguments.cmake_variable:
        name, separator, value = entry.partition("=")
        if (
            not separator
            or not SAFE_CMAKE_VARIABLE_NAME.fullmatch(name)
            or not value
            or "\n" in value
            or "\r" in value
        ):
            raise RunnerError(f"invalid CMake variable: {entry}")
        cmake_variables.append((name, value))
    try:
        environment = require_environment(arguments)
    except NativeRuntimeError as error:
        raise RunnerError(str(error)) from error
    idf_path, idf_py, idf_python, compiler, ninja = validate_tools(
        environment,
        arguments.target,
    )
    expected_idf_commit = read_expected_commit(
        arguments.idf_version_file,
        "ESP-IDF",
    )
    validate_commit(idf_path, expected_idf_commit, environment)
    expected_tool_versions = read_expected_tool_versions(
        arguments.idf_tool_versions_file,
    )
    validate_tool_versions(
        idf_path,
        idf_python,
        compiler,
        ninja,
        arguments.target,
        expected_tool_versions,
        environment,
    )
    source_root = Path(os.path.abspath(arguments.source_root))
    prebuilt_components = resolve_prebuilt_components(
        source_root,
        arguments.prebuilt_component,
    )
    prebuilt_component_includes = resolve_prebuilt_component_includes(
        source_root,
        arguments.prebuilt_component_include,
        prebuilt_components,
    )
    native_components = resolve_native_components(
        source_root,
        arguments.native_component,
    )
    native_component_sources = resolve_native_component_sources(
        source_root,
        arguments.native_component_source,
    )
    source_project = resolve_project(
        source_root,
        Path(arguments.project),
        arguments.target,
    )
    try:
        relative_project = source_project.relative_to(source_root)
    except ValueError as error:
        raise RunnerError(f"launcher project escapes source root: {source_project}") from error

    subprocess_environment = dict(environment)
    subprocess_environment["ESP_IDF_VERSION"] = EXPECTED_IDF_VERSION
    subprocess_environment["H2_REPO_ROOT"] = str(source_root)
    subprocess_environment["H2_GIZOS_ROOT"] = str(GIZOS_ROOT)
    subprocess_environment["H2_FIRMWARE_VERSION"] = arguments.version
    subprocess_environment["H2_BAZEL_NATIVE_ARTIFACTS_ONLY"] = "1"
    prebuilt_include_paths = [
        path
        for paths in prebuilt_component_includes.values()
        for path in paths
    ]
    for variable, repository_name in (
        ("H2_PIXA_UPSTREAM_ROOT", "h2_vendor_pixa"),
        ("H2_PIXELROOT32_UPSTREAM_ROOT", "h2_vendor_pixelroot32"),
    ):
        try:
            repository_root = find_external_repository_source_root(
                prebuilt_include_paths, repository_name
            )
        except NativeRuntimeError as error:
            raise RunnerError(str(error)) from error
        if repository_root is not None:
            subprocess_environment[variable] = str(repository_root)
    with tempfile.TemporaryDirectory(prefix=f"h2-esp-idf-{arguments.target}-") as temporary:
        temporary_root = Path(temporary)
        wrapper_directory = create_ninja_wrapper(
            temporary_root,
            ninja,
            environment[NATIVE_BUILD_JOBS],
        )
        try:
            ccache = configure_ccache_environment(
                subprocess_environment,
                arguments.target,
                temporary_root,
                arguments.ccache_runtime_locator,
            )
        except NativeCcacheError as error:
            raise RunnerError(str(error)) from error
        if ccache is not None:
            wrapper_directory.joinpath("ccache").symlink_to(ccache)
            subprocess_environment["IDF_CCACHE_ENABLE"] = "1"
            if arguments.h2loader_wifi_environment:
                subprocess_environment["CCACHE_READONLY"] = "1"
        subprocess_environment["PATH"] = (
            f"{wrapper_directory}{os.pathsep}{environment['PATH']}"
        )
        # ESP-IDF 6.0 constrains idf-component-manager to 3.0.x. Its
        # ComponentManagerSettings uses the IDF_COMPONENT_ prefix with the
        # CACHE_PATH field, which maps to IDF_COMPONENT_CACHE_PATH.
        subprocess_environment["IDF_COMPONENT_CACHE_PATH"] = str(
            temporary_root / "component-cache"
        )
        project_copy = temporary_root / "source" / relative_project
        project_copy.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source_project, project_copy)
        partition = required_file(
            source_root / arguments.partition,
            "firmware partition table",
        )
        copy_output(partition, project_copy / "partition.csv")
        for value in arguments.project_support_file:
            support_file = required_file(
                source_root / value,
                "launcher project support file",
            )
            destination = project_copy / support_file.name
            if destination.exists():
                raise RunnerError(
                    f"launcher project support file collides with project input: {support_file.name}"
                )
            copy_output(support_file, destination)
        copy_support_files(
            source_root,
            temporary_root / "source",
            arguments.support_file,
        )
        build_directory = temporary_root / "build"
        component_directories: dict[str, Path] = {}
        component_alias_root = temporary_root / "components"
        archive_helper = (
            GIZOS_ROOT
            / "native_component_src/esp-idf6.x/cmake/h2_bazel_archive.cmake"
        )
        if arguments.generate_prebuilt_component:
            archive_helper = required_file(archive_helper, "ESP-IDF Bazel archive helper")
        for component_name in sorted(arguments.generate_prebuilt_component):
            if component_name not in prebuilt_components:
                raise RunnerError(f"unknown generated ESP-IDF prebuilt component: {component_name}")
            if component_name in native_components:
                raise RunnerError(
                    f"ESP-IDF component {component_name} is both native and prebuilt"
                )
            generated = component_alias_root / component_name
            generated.mkdir(parents=True, exist_ok=True)
            (generated / "CMakeLists.txt").write_text(
                render_prebuilt_component(
                    component_name,
                    prebuilt_component_includes[component_name],
                    archive_helper,
                ),
                encoding="utf-8",
            )
            component_directories[component_name] = generated
        for component_name, component_directory in sorted(native_components.items()):
            if component_directory.name == component_name:
                component_directories[component_name] = component_directory
                continue
            component_alias_root.mkdir(parents=True, exist_ok=True)
            alias = component_alias_root / component_name
            alias.symlink_to(component_directory, target_is_directory=True)
            component_directories[component_name] = alias
        component_manifest = project_copy / "h2_bazel_components.cmake"
        component_manifest.write_text(
            render_native_component_manifest(
                arguments.board,
                component_directories,
                native_component_sources,
            ),
            encoding="utf-8",
        )
        subprocess_environment["H2_BAZEL_COMPONENT_MANIFEST"] = str(component_manifest)
        command = [
            str(idf_py),
            "-C",
            str(project_copy),
            "-B",
            str(build_directory),
            "-D",
            f"H2_REPO_ROOT={source_root}",
            "-D",
            f"PROJECT_VER={arguments.version}",
        ]
        for name, value in cmake_variables:
            command.extend(["-D", f"{name}={value}"])
        for component_name, archive in sorted(prebuilt_components.items()):
            command.extend([
                "-D",
                "H2_BAZEL_PREBUILT_%s=%s" % (component_name.upper(), archive),
            ])
        command.append("build")
        print(
            "ESP-IDF build starting: "
            f"launcher={relative_project.as_posix()} "
            f"target={arguments.target} "
            f"sdk_commit={expected_idf_commit} "
            f"build_dir={build_directory}"
        )
        try:
            result = subprocess.run(
                command,
                check=False,
                env=subprocess_environment,
            )
        except OSError as error:
            raise RunnerError(f"cannot execute ESP-IDF build: {error}") from error
        if result.returncode != 0:
            raise RunnerError(f"idf.py build failed with exit code {result.returncode}")
        combined_factory_image = build_directory / "combined_factory.bin"
        merge_command = command[:-1] + [
            "merge-bin",
            "--output",
            str(combined_factory_image),
        ]
        try:
            merge_result = subprocess.run(
                merge_command,
                check=False,
                env=subprocess_environment,
            )
        except OSError as error:
            raise RunnerError(f"cannot execute ESP-IDF merge-bin: {error}") from error
        if merge_result.returncode != 0:
            raise RunnerError(
                "idf.py merge-bin failed with exit code "
                f"{merge_result.returncode}"
            )
        publish_outputs(build_directory, arguments.project_name, arguments)


def main(argv: list[str]) -> int:
    try:
        run(parse_arguments(argv))
    except (RunnerError, OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
