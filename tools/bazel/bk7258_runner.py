#!/usr/bin/env python3
"""Run one local BK7258 build and publish validated native artifacts."""

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
    fixed_environment,
    locator_path,
    read_locator,
)


NATIVE_BUILD_JOBS = "H2_NATIVE_BUILD_JOBS"
SAFE_PROJECT_NAME = re.compile(r"^[A-Za-z0-9_]+$")
SAFE_COMPONENT_NAME = re.compile(r"^[A-Za-z0-9_]+$")
COMMIT = re.compile(r"^[0-9a-f]{40}$")
LAUNCHER_TARGET = re.compile(
    r'^\s*set\(\s*H2_BK_TARGET\s+"([^"]+)"',
    re.MULTILINE,
)
PARTITION_METADATA = (
    "partitions.json",
    "bk_ota_partitions.json",
    "bk_package.json",
    "configurationab.json",
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


def require_environment(arguments: argparse.Namespace) -> dict[str, str]:
    sdk_locator = read_locator(arguments.sdk_locator, "bk7258-sdk")
    toolchain_locator = read_locator(arguments.toolchain_locator, "bk-arm-toolchain")
    sdk = locator_path(sdk_locator, "root", "bk7258-sdk")
    toolchain = locator_path(toolchain_locator, "bin", "bk-arm-toolchain")
    environment = fixed_environment([toolchain])
    environment.update({
        "BK7258_PATH": str(sdk),
        "COMPILER_TOOLCHAIN_PATH": str(toolchain),
        NATIVE_BUILD_JOBS: "4",
    })
    return environment


def require_executable(path: Path, label: str) -> Path:
    if not path.is_file() or not os.access(path, os.X_OK):
        raise RunnerError(f"{label} is unavailable or not executable: {path}")
    return path


def find_executable(name: str, environment: dict[str, str]) -> Path:
    result = shutil.which(name, path=environment["PATH"])
    if result is None:
        raise RunnerError(f"required executable is unavailable in PATH: {name}")
    return require_executable(Path(result), name)


def resolve_python_environment(
    environment: dict[str, str], expected_version: str
) -> Path:
    compatibility_check = (
        "from distutils.dir_util import copy_tree\n"
        "import click\n"
        "import click_option_group\n"
        "import cryptography\n"
        "import Crypto\n"
        "import sys\n"
        'print(f"{sys.version_info.major}.{sys.version_info.minor}")\n'
    )
    executable = require_executable(Path(sys.executable).resolve(), "Bazel Python")
    try:
        result = subprocess.run(
            [str(executable), "-c", compatibility_check],
            check=False,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except OSError as error:
        raise RunnerError(f"cannot inspect BK7258 python: {error}") from error
    if result.returncode != 0:
        detail = result.stderr.strip() or "compatibility check failed"
        raise RunnerError(f"BK7258 python is incompatible: {detail}")
    actual_version = result.stdout.strip()
    if actual_version != expected_version:
        raise RunnerError(
            "BK7258 python version mismatch: "
            f"expected {expected_version}, found {actual_version or 'unknown'}"
        )
    return executable


def bind_python_dependencies(environment: dict[str, str]) -> None:
    dependency_paths = [path for path in sys.path if "bk7258_pip" in path]
    if not dependency_paths:
        return
    inherited = environment.get("PYTHONPATH")
    if inherited:
        dependency_paths.append(inherited)
    environment["PYTHONPATH"] = os.pathsep.join(dependency_paths)


def bind_python_environment(
    environment: dict[str, str], python: Path, tool_directory: Path
) -> None:
    tool_directory.mkdir()
    for name in ("python", "python3"):
        wrapper = tool_directory / name
        wrapper.write_text(
            "#!/bin/sh\nexec " + shlex.quote(str(python)) + ' "$@"\n',
            encoding="utf-8",
        )
        wrapper.chmod(0o755)
    environment["PATH"] = f"{tool_directory}{os.pathsep}{environment['PATH']}"


def validate_toolchain(
    environment: dict[str, str], expected_version: str
) -> tuple[Path, Path, Path]:
    sdk_path = Path(environment["BK7258_PATH"]).resolve()
    if not (sdk_path / "Makefile").is_file():
        raise RunnerError(f"BK7258 SDK Makefile is missing: {sdk_path / 'Makefile'}")
    toolchain_path = Path(environment["COMPILER_TOOLCHAIN_PATH"]).resolve()
    compiler = require_executable(
        toolchain_path / "arm-none-eabi-gcc",
        "BK7258 compiler",
    )
    try:
        result = subprocess.run(
            [str(compiler), "-dumpfullversion", "-dumpversion"],
            check=False,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except OSError as error:
        raise RunnerError(f"cannot inspect BK7258 compiler: {error}") from error
    actual_version = result.stdout.strip() if result.returncode == 0 else "unknown"
    if actual_version != expected_version:
        raise RunnerError(
            "BK7258 compiler version mismatch: "
            f"expected {expected_version}, found {actual_version}"
        )
    return sdk_path, find_executable("git", environment), find_executable("make", environment)


def run_git(
    git: Path,
    checkout: Path,
    arguments: list[str],
    environment: dict[str, str],
) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            [str(git), "-C", str(checkout), *arguments],
            check=False,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except OSError as error:
        raise RunnerError(f"cannot inspect Git checkout {checkout}: {error}") from error


def require_git_output(
    git: Path,
    checkout: Path,
    arguments: list[str],
    environment: dict[str, str],
    label: str,
) -> str:
    result = run_git(git, checkout, arguments, environment)
    if result.returncode != 0:
        detail = result.stderr.strip() or "unknown Git error"
        raise RunnerError(f"cannot inspect {label}: {detail}")
    return result.stdout


def resolve_source_checkout(
    source_root: Path,
    project_file: Path,
    git: Path,
    environment: dict[str, str],
) -> Path:
    resolved_project = (source_root / project_file).resolve()
    repository = Path(
        require_git_output(
            git,
            resolved_project.parent,
            ["rev-parse", "--show-toplevel"],
            environment,
            "Firmwares source checkout",
        ).strip()
    ).resolve()
    if (repository / project_file).resolve() != resolved_project:
        raise RunnerError(
            "launcher project does not resolve inside the Firmwares source checkout: "
            f"{resolved_project}"
        )
    return repository


def tracked_state(
    git: Path,
    checkout: Path,
    environment: dict[str, str],
    label: str,
) -> tuple[str, str, str]:
    head = require_git_output(
        git,
        checkout,
        ["rev-parse", "HEAD"],
        environment,
        label,
    ).strip()
    working_tree = require_git_output(
        git,
        checkout,
        ["diff", "--binary", "--no-ext-diff", "--no-textconv", "--"],
        environment,
        label,
    )
    index = require_git_output(
        git,
        checkout,
        ["diff", "--cached", "--binary", "--no-ext-diff", "--no-textconv", "--"],
        environment,
        label,
    )
    return head, working_tree, index


def validate_tracked_state(
    git: Path,
    checkout: Path,
    expected: tuple[str, str, str],
    environment: dict[str, str],
    label: str,
) -> None:
    if tracked_state(git, checkout, environment, label) != expected:
        raise RunnerError(f"{label} tracked state changed during the native build")


def validate_sdk(
    sdk_path: Path,
    git: Path,
    expected_commit: str,
    environment: dict[str, str],
) -> None:
    if not COMMIT.fullmatch(expected_commit):
        raise RunnerError(f"invalid expected BK7258 SDK commit: {expected_commit}")
    result = run_git(git, sdk_path, ["rev-parse", "HEAD"], environment)
    actual_commit = result.stdout.strip() if result.returncode == 0 else "unknown"
    if actual_commit != expected_commit:
        raise RunnerError(
            "BK7258 SDK commit mismatch: "
            f"expected {expected_commit}, found {actual_commit}"
        )
    checks = (
        ("working tree", ["diff", "--quiet", "--"]),
        ("index", ["diff", "--cached", "--quiet", "--"]),
    )
    for label, arguments in checks:
        result = run_git(git, sdk_path, arguments, environment)
        if result.returncode != 0:
            raise RunnerError(f"BK7258 SDK has tracked modifications in the {label}")


def resolve_project(source_root: Path, project_file: Path, target: str) -> Path:
    if project_file.is_absolute() or ".." in project_file.parts:
        raise RunnerError(f"launcher project must be source-root relative: {project_file}")
    project_file = source_root / project_file
    if project_file.name != "CMakeLists.txt" or not project_file.is_file():
        raise RunnerError(f"launcher project CMakeLists.txt is missing: {project_file}")
    matches = LAUNCHER_TARGET.findall(project_file.read_text(encoding="utf-8"))
    if len(matches) != 1:
        raise RunnerError(
            f"expected one H2_BK_TARGET in {project_file}, found {len(matches)}"
        )
    if matches[0] != target:
        raise RunnerError(f"launcher target mismatch: rule={target}, CMake={matches[0]}")
    return project_file.parent


def required_file(path: Path, label: str, build_root: Path | None = None) -> Path:
    resolved = path.resolve()
    if build_root is not None:
        try:
            resolved.relative_to(build_root.resolve())
        except ValueError as error:
            raise RunnerError(f"BK7258 output escapes build directory: {label}: {path}") from error
    if not resolved.is_file():
        raise RunnerError(f"required BK7258 output is missing: {label}: {path}")
    if resolved.stat().st_size == 0:
        raise RunnerError(f"required BK7258 output is empty: {label}: {path}")
    return resolved


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


CONFIG_LINE = re.compile(r"^(?:# )?(CONFIG_[A-Za-z0-9_]+)(?:=.*| is not set)$")


def merge_config_layers(layers: list[Path], destination: Path) -> None:
    """Merge Kconfig assignments in declaration order, with later layers winning."""
    assignments: dict[str, str] = {}
    order: list[str] = []
    for layer in layers:
        for line in layer.read_text(encoding="utf-8").splitlines():
            match = CONFIG_LINE.fullmatch(line)
            if not match:
                continue
            symbol = match.group(1)
            if symbol not in assignments:
                order.append(symbol)
            assignments[symbol] = line
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(
        "# Generated from the declared AP/CP configuration inputs.\n"
        + "\n".join(assignments[symbol] for symbol in order)
        + "\n",
        encoding="utf-8",
    )


def stage_unit_inputs(
    source_root: Path,
    project_copy: Path,
    arguments: argparse.Namespace,
) -> None:
    units = {
        "ap": (arguments.ap_config, arguments.ap_gpio, project_copy / "ap/config/bk7258_ap"),
        "cp": (arguments.cp_config, arguments.cp_gpio, project_copy / "cp/config/bk7258"),
    }
    for unit, (configs, gpio, directory) in units.items():
        layers = [
            required_file(source_root / value, f"BK7258 {unit.upper()} configuration")
            for value in configs
        ]
        if not layers:
            raise RunnerError(f"BK7258 {unit.upper()} config has no declared inputs")
        image_config = directory / "config"
        if image_config.is_file():
            layers.append(image_config)
        merge_config_layers(layers, image_config)
        gpio_file = required_file(source_root / gpio, f"BK7258 {unit.upper()} GPIO configuration")
        copy_output(gpio_file, directory / "usr_gpio_cfg.h")


def validate_partition_metadata(
    metadata_directory: Path,
    build_root: Path,
) -> list[tuple[str, Path]]:
    sources: list[tuple[str, Path]] = []
    for name in PARTITION_METADATA:
        source = required_file(metadata_directory / name, name, build_root)
        try:
            metadata = json.loads(source.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise RunnerError(f"invalid BK7258 partition metadata {name}: {error}") from error
        if not isinstance(metadata, dict):
            raise RunnerError(f"BK7258 partition metadata must be a JSON object: {name}")
        sources.append((name, source))
    return sources


def publish_outputs(
    build_directory: Path,
    native_project: str,
    target: str,
    arguments: argparse.Namespace,
) -> None:
    native_root = build_directory / target / native_project
    ap_root = native_root / f"{target}_ap"
    cp_root = native_root / target
    package_root = native_root / "package"
    native_outputs = {
        "AP ELF": ap_root / "app.elf",
        "AP map": ap_root / "app.map",
        "AP image": ap_root / "app.bin",
        "CP ELF": cp_root / "app.elf",
        "CP map": cp_root / "app.map",
        "CP image": cp_root / "app.bin",
        "managed app image": package_root / "app_ab_crc.rbl",
        "recovery image": package_root / "all-app.bin",
    }
    for label, path in native_outputs.items():
        native_outputs[label] = required_file(path, label, build_directory)
    metadata_sources = validate_partition_metadata(
        native_root / "partitions",
        build_directory,
    )

    metadata_output = Path(arguments.partition_metadata_output)
    if metadata_output.exists():
        if not metadata_output.is_dir() or any(metadata_output.iterdir()):
            raise RunnerError(
                f"partition metadata output is not an empty directory: {metadata_output}"
            )
    else:
        metadata_output.mkdir(parents=True)
    for name, source in metadata_sources:
        copy_output(source, metadata_output / name)

    destinations = {
        "AP ELF": Path(arguments.ap_elf_output),
        "AP map": Path(arguments.ap_map_output),
        "AP image": Path(arguments.ap_image_output),
        "CP ELF": Path(arguments.cp_elf_output),
        "CP map": Path(arguments.cp_map_output),
        "CP image": Path(arguments.cp_image_output),
        "managed app image": Path(arguments.managed_app_output),
        "recovery image": Path(arguments.recovery_output),
    }
    for label, destination in destinations.items():
        copy_output(native_outputs[label], destination)


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--project", required=True)
    parser.add_argument("--project-name", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--board", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--sdk-version-file", required=True)
    parser.add_argument("--toolchain-archives-file", required=True)
    parser.add_argument("--sdk-locator", required=True)
    parser.add_argument("--toolchain-locator", required=True)
    parser.add_argument("--ccache-runtime-locator", required=True)
    parser.add_argument("--expected-python-version", required=True)
    parser.add_argument("--expected-toolchain-version", required=True)
    parser.add_argument("--ap-elf-output", required=True)
    parser.add_argument("--ap-map-output", required=True)
    parser.add_argument("--ap-image-output", required=True)
    parser.add_argument("--cp-elf-output", required=True)
    parser.add_argument("--cp-map-output", required=True)
    parser.add_argument("--cp-image-output", required=True)
    parser.add_argument("--managed-app-output", required=True)
    parser.add_argument("--recovery-output", required=True)
    parser.add_argument("--partition-metadata-output", required=True)
    parser.add_argument("--ram-regions", required=True)
    parser.add_argument("--memory-contract")
    parser.add_argument("--memory-checker")
    parser.add_argument("--support-file", action="append", default=[])
    parser.add_argument("--project-support-file", action="append", default=[])
    parser.add_argument("--ap-config", action="append", default=[])
    parser.add_argument("--cp-config", action="append", default=[])
    parser.add_argument("--ap-gpio", required=True)
    parser.add_argument("--cp-gpio", required=True)
    parser.add_argument("--prebuilt-component", action="append", default=[])
    parser.add_argument("--prebuilt-component-include", action="append", default=[])
    parser.add_argument("--generate-prebuilt-component", action="append", default=[])
    parser.add_argument("--native-component", action="append", default=[])
    parser.add_argument("--native-component-file", action="append", default=[])
    parser.add_argument("--native-component-source", action="append", default=[])
    return parser.parse_args(argv)


def parse_native_key(value: str, label: str) -> tuple[str, str, str]:
    key, separator, payload = value.partition("=")
    execution_unit, key_separator, component_name = key.partition(":")
    if (
        not separator
        or not key_separator
        or execution_unit not in ("ap", "cp")
        or not SAFE_COMPONENT_NAME.fullmatch(component_name)
        or not payload
    ):
        raise RunnerError(f"invalid BK7258 {label}: {value}")
    return execution_unit, component_name, payload


def resolve_native_components(
    source_root: Path,
    definitions: list[str],
) -> dict[tuple[str, str], Path]:
    components: dict[tuple[str, str], Path] = {}
    for definition in definitions:
        execution_unit, component_name, value = parse_native_key(
            definition, "native component"
        )
        relative = Path(value)
        if relative.is_absolute() or ".." in relative.parts:
            raise RunnerError(f"BK7258 native component escapes source root: {value}")
        directory = (source_root / relative).resolve()
        if not directory.is_dir() or not (directory / "CMakeLists.txt").is_file():
            raise RunnerError(f"BK7258 native component is invalid: {value}")
        key = (execution_unit, component_name)
        if key in components:
            raise RunnerError(f"duplicate BK7258 native component: {execution_unit}:{component_name}")
        components[key] = directory
    return components


def resolve_native_component_sources(
    source_root: Path,
    definitions: list[str],
) -> dict[tuple[str, str], list[Path]]:
    components: dict[tuple[str, str], list[Path]] = {}
    for definition in definitions:
        execution_unit, component_name, value = parse_native_key(
            definition, "native component source"
        )
        relative = Path(value)
        if relative.is_absolute() or ".." in relative.parts:
            raise RunnerError(f"BK7258 native component source escapes source root: {value}")
        source = required_file(
            source_root / relative,
            f"native component {execution_unit}:{component_name} source",
        )
        sources = components.setdefault((execution_unit, component_name), [])
        if source not in sources:
            sources.append(source)
    return components


def resolve_native_component_files(
    source_root: Path,
    definitions: list[str],
) -> dict[tuple[str, str], list[Path]]:
    components: dict[tuple[str, str], list[Path]] = {}
    for definition in definitions:
        execution_unit, component_name, value = parse_native_key(
            definition, "native component file"
        )
        relative = Path(value)
        if relative.is_absolute() or ".." in relative.parts:
            raise RunnerError(f"BK7258 native component file escapes source root: {value}")
        component_file = required_file(
            source_root / relative,
            f"native component {execution_unit}:{component_name} file",
        )
        files = components.setdefault((execution_unit, component_name), [])
        if component_file not in files:
            files.append(component_file)
    return components


def resolve_prebuilt_components(
    source_root: Path, values: list[str]
) -> dict[str, Path]:
    source_root = source_root.resolve()
    components: dict[str, Path] = {}
    for value in values:
        name, separator, raw_path = value.partition("=")
        if (
            not separator
            or not SAFE_COMPONENT_NAME.fullmatch(name)
            or name in components
        ):
            raise RunnerError(f"invalid BK7258 prebuilt component: {value}")
        relative_archive = Path(raw_path)
        if (
            not raw_path
            or relative_archive.is_absolute()
            or ".." in relative_archive.parts
            or relative_archive.suffix != ".a"
        ):
            raise RunnerError(f"invalid BK7258 prebuilt archive path: {raw_path}")
        archive = (source_root / relative_archive).resolve()
        try:
            archive.relative_to(source_root)
        except ValueError as error:
            raise RunnerError(
                f"BK7258 prebuilt archive escapes source root: {raw_path}"
            ) from error
        if not archive.is_file():
            raise RunnerError(f"BK7258 prebuilt component archive is missing: {archive}")
        components[name] = archive
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
            raise RunnerError(f"invalid BK7258 prebuilt component include: {value}")
        include = source_root / relative_path
        if not include.is_dir():
            raise RunnerError(f"BK7258 prebuilt component include is not a directory: {raw_path}")
        if include not in includes[component_name]:
            includes[component_name].append(include)
    return includes


def render_prebuilt_component(
    component_name: str,
    includes: list[Path],
    archive_helper: Path,
) -> str:
    lines = ["armino_component_register("]
    if includes:
        lines.append("  INCLUDE_DIRS")
        lines.extend(f'    "{include.as_posix()}"' for include in includes)
    lines.extend([
        ")",
        "",
        f'include("{archive_helper.as_posix()}")',
        f"h2_bk_import_bazel_archive({component_name} H2_BAZEL_PREBUILT_{component_name.upper()})",
        "",
    ])
    return "\n".join(lines)


def run(arguments: argparse.Namespace) -> None:
    if not SAFE_PROJECT_NAME.fullmatch(arguments.project_name):
        raise RunnerError(f"invalid BK7258 project name: {arguments.project_name}")
    if arguments.target != "bk7258":
        raise RunnerError(f"unsupported BK target: {arguments.target}")
    if bool(arguments.memory_contract) != bool(arguments.memory_checker):
        raise RunnerError("memory contract and checker must be declared together")
    try:
        environment = require_environment(arguments)
    except NativeRuntimeError as error:
        raise RunnerError(str(error)) from error
    bind_python_dependencies(environment)
    python = resolve_python_environment(
        environment,
        arguments.expected_python_version,
    )
    sdk_path, git, make = validate_toolchain(
        environment,
        arguments.expected_toolchain_version,
    )
    expected_sdk_commit = read_expected_commit(
        arguments.sdk_version_file,
        "BK7258 SDK",
    )
    if not Path(arguments.toolchain_archives_file).is_file():
        raise RunnerError("BK ARM toolchain archive identity is missing")
    validate_sdk(sdk_path, git, expected_sdk_commit, environment)
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
    project_file = Path(arguments.project)
    source_project = resolve_project(source_root, project_file, arguments.target)
    native_components = resolve_native_components(
        source_root,
        arguments.native_component,
    )
    native_component_files = resolve_native_component_files(
        source_root,
        arguments.native_component_file,
    )
    native_component_sources = resolve_native_component_sources(
        source_root,
        arguments.native_component_source,
    )
    for key in sorted(set(native_component_files) | set(native_component_sources)):
        if key not in native_components:
            raise RunnerError(
                "BK7258 native component input has no component descriptor: "
                f"{key[0]}:{key[1]}"
            )
    source_checkout = resolve_source_checkout(
        source_root,
        project_file,
        git,
        environment,
    )
    source_state = tracked_state(
        git,
        source_checkout,
        environment,
        "Firmwares source checkout",
    )
    try:
        relative_project = source_project.relative_to(source_root)
    except ValueError as error:
        raise RunnerError(f"launcher project escapes source root: {source_project}") from error

    with tempfile.TemporaryDirectory(prefix="h2-bk7258-") as temporary:
        temporary_root = Path(temporary)
        project_copy = temporary_root / "source" / relative_project
        project_copy.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source_project, project_copy)
        copy_support_files(source_root, temporary_root / "source", arguments.support_file)
        for value in arguments.project_support_file:
            support_file = required_file(source_root / value, "BK7258 project support file")
            parts = support_file.parts
            if "layouts" not in parts:
                raise RunnerError(f"BK7258 project support file is outside board layouts: {support_file}")
            layout_index = parts.index("layouts")
            if layout_index + 2 >= len(parts):
                raise RunnerError(f"BK7258 project support file has an invalid board layout path: {support_file}")
            relative = Path(*parts[layout_index + 2:])
            copy_output(support_file, project_copy / relative)
        stage_unit_inputs(
            source_root,
            project_copy,
            arguments,
        )
        copy_output(
            required_file(source_root / arguments.ram_regions, "BK7258 RAM regions"),
            project_copy / "partitions/bk7258/ram_regions.csv",
        )
        build_directory = temporary_root / "build"
        version_file = temporary_root / "firmware-version.txt"
        version_file.write_text(arguments.version, encoding="utf-8")
        subprocess_environment = dict(environment)
        bind_python_environment(
            subprocess_environment,
            python,
            temporary_root / "python-bin",
        )
        subprocess_environment.update(
            {
                "H2_FIRMWARE_VERSION_FILE": str(version_file),
                "H2_REPO_ROOT": str(source_root),
            }
        )
        try:
            ccache = configure_ccache_environment(
                subprocess_environment,
                arguments.target,
                temporary_root,
                arguments.ccache_runtime_locator,
            )
            if ccache is not None:
                temporary_root.joinpath("python-bin/ccache").symlink_to(ccache)
                subprocess_environment["ARMINO_CCACHE_ENABLE"] = "1"
        except NativeCcacheError as error:
            raise RunnerError(str(error)) from error
        staged_components: dict[tuple[str, str], Path] = {}
        archive_helper = source_root / "native_component_src/bk7258/ap/cmake/h2_bazel_archive.cmake"
        if arguments.generate_prebuilt_component:
            archive_helper = required_file(archive_helper, "BK7258 Bazel archive helper")
        for component_name in sorted(arguments.generate_prebuilt_component):
            if component_name not in prebuilt_components:
                raise RunnerError(f"unknown generated BK7258 prebuilt component: {component_name}")
            key = ("ap", component_name)
            if key in native_components:
                raise RunnerError(
                    f"BK7258 component ap:{component_name} is both native and prebuilt"
                )
            generated = temporary_root / "components" / "ap" / component_name
            generated.mkdir(parents=True, exist_ok=True)
            (generated / "CMakeLists.txt").write_text(
                render_prebuilt_component(
                    component_name,
                    prebuilt_component_includes[component_name],
                    archive_helper,
                ),
                encoding="utf-8",
            )
            staged_components[key] = generated
        for key, directory in sorted(native_components.items()):
            execution_unit, component_name = key
            try:
                directory.relative_to(source_project)
            except ValueError:
                staged_directory = (
                    temporary_root
                    / "components"
                    / execution_unit
                    / component_name
                )
                staged_directory.mkdir(parents=True)
                for component_file in native_component_files.get(key, []):
                    try:
                        relative_file = component_file.relative_to(directory)
                    except ValueError:
                        continue
                    copy_output(component_file, staged_directory / relative_file)
                if not (staged_directory / "CMakeLists.txt").is_file():
                    raise RunnerError(
                        "BK7258 native component does not declare its CMakeLists.txt: "
                        f"{execution_unit}:{component_name}"
                    )
                staged_components[key] = staged_directory
            else:
                staged_components[key] = directory
        manifest_lines = ["# Generated from the Bazel native component graph."]
        for execution_unit in ("ap", "cp"):
            manifest_lines.append(f"set(H2_BAZEL_{execution_unit.upper()}_COMPONENT_DIRS")
            for (unit, component_name), directory in sorted(staged_components.items()):
                if unit != execution_unit:
                    continue
                try:
                    directory.relative_to(source_project)
                except ValueError:
                    manifest_lines.append(f'  "{directory.as_posix()}"')
            manifest_lines.append(")")
        for (execution_unit, component_name), _directory in sorted(native_components.items()):
            variable = f"H2_BAZEL_COMPONENT_SRCS_{execution_unit}_{component_name}".upper()
            manifest_lines.append(f"set({variable}")
            for source in sorted(native_component_sources.get((execution_unit, component_name), [])):
                manifest_lines.append(f'  "{source.as_posix()}"')
            manifest_lines.append(")")
        component_manifest = project_copy / "h2_bazel_components.cmake"
        component_manifest.write_text("\n".join(manifest_lines) + "\n", encoding="utf-8")
        subprocess_environment["H2_BAZEL_COMPONENT_MANIFEST"] = str(component_manifest)
        for component_name, archive in sorted(prebuilt_components.items()):
            subprocess_environment[
                f"H2_BAZEL_PREBUILT_{component_name.upper()}"
            ] = str(archive)
        command = [
            str(make),
            f"-j{environment[NATIVE_BUILD_JOBS]}",
            "-C",
            str(sdk_path),
            arguments.target,
            f"PROJECT={arguments.project_name}",
            f"PROJECT_DIR={project_copy}",
            f"BUILD_DIR={build_directory}",
            f"COMPILER_TOOLCHAIN_PATH={environment['COMPILER_TOOLCHAIN_PATH']}",
        ]
        memory_report_root = temporary_root / "memory-reports"
        if arguments.memory_contract:
            source_check = subprocess.run(
                [
                    arguments.memory_checker,
                    "source",
                    "--contract",
                    str(source_root / arguments.memory_contract),
                    "--repo-root",
                    str(source_root),
                    "--image-root",
                    str(project_copy),
                    "--report",
                    str(memory_report_root / "source.json"),
                ],
                check=False,
                env=subprocess_environment,
            )
            if source_check.returncode != 0:
                raise RunnerError("BK7258 source memory contract validation failed")
        print(
            "BK7258 build starting: "
            f"launcher={relative_project.as_posix()} "
            f"target={arguments.target} "
            f"sdk_commit={expected_sdk_commit} "
            f"python_version={arguments.expected_python_version} "
            f"toolchain_version={arguments.expected_toolchain_version} "
            f"build_dir={build_directory}"
        )
        try:
            result = subprocess.run(command, check=False, env=subprocess_environment)
        except OSError as error:
            raise RunnerError(f"cannot execute BK7258 build: {error}") from error
        validate_sdk(sdk_path, git, expected_sdk_commit, environment)
        validate_tracked_state(
            git,
            source_checkout,
            source_state,
            environment,
            "Firmwares source checkout",
        )
        if result.returncode != 0:
            raise RunnerError(f"BK7258 make failed with exit code {result.returncode}")
        publish_outputs(
            build_directory,
            project_copy.name,
            arguments.target,
            arguments,
        )
        if arguments.memory_contract:
            post_link_check = subprocess.run(
                [
                    arguments.memory_checker,
                    "post-link",
                    "--contract",
                    str(source_root / arguments.memory_contract),
                    "--repo-root",
                    str(source_root),
                    "--image-root",
                    str(project_copy),
                    "--map",
                    arguments.ap_map_output,
                    "--elf",
                    arguments.ap_elf_output,
                    "--nm",
                    str(Path(environment["COMPILER_TOOLCHAIN_PATH"]) / "arm-none-eabi-nm"),
                    "--report",
                    str(memory_report_root / "post-link.json"),
                ],
                check=False,
                env=subprocess_environment,
            )
            if post_link_check.returncode != 0:
                raise RunnerError("BK7258 post-link memory contract validation failed")


def main(argv: list[str]) -> int:
    try:
        run(parse_arguments(argv))
    except (RunnerError, OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
