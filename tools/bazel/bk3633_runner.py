#!/usr/bin/env python3
"""Build and normalize one BK3633 firmware release."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.bazel.native_ccache import (
    NativeCcacheError,
    configure_environment as configure_ccache_environment,
    create_wrapped_toolchain,
)
from tools.bazel.native_runtime import (
    NativeRuntimeError,
    fixed_environment,
    locator_path,
    read_locator,
)


class RunnerError(RuntimeError):
    """The native BK3633 build contract was not satisfied."""


COMMIT = re.compile(r"^[0-9a-f]{40}$")
COMPONENT_NAME = re.compile(r"^[a-z][a-z0-9_]*$")
GIZOS_ROOT = Path(__file__).resolve().parents[2]


def read_expected_commit(path: str, label: str) -> str:
    try:
        commit = Path(path).read_text(encoding="utf-8").strip()
    except OSError as error:
        raise RunnerError(f"cannot read {label} version file: {path}: {error}") from error
    if not COMMIT.fullmatch(commit):
        raise RunnerError(f"invalid expected {label} commit: {commit}")
    return commit


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--project", required=True)
    parser.add_argument("--entry", required=True)
    parser.add_argument("--board", required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--native-target", required=True)
    parser.add_argument("--native-merge", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--sdk-version-file", required=True)
    parser.add_argument("--toolchain-archives-file", required=True)
    parser.add_argument("--sdk-locator", required=True)
    parser.add_argument("--toolchain-locator", required=True)
    parser.add_argument("--ccache-runtime-locator", required=True)
    parser.add_argument("--expected-toolchain-version", required=True)
    parser.add_argument("--binconverter", required=True)
    parser.add_argument("--elf-output", required=True)
    parser.add_argument("--app-output", required=True)
    parser.add_argument("--map-output", required=True)
    parser.add_argument("--recovery-output", required=True)
    parser.add_argument("--manifest-output", required=True)
    parser.add_argument("--prebuilt-component", action="append", default=[])
    parser.add_argument("--native-component-source", action="append", default=[])
    parser.add_argument("--native-include-root", action="append", default=[])
    parser.add_argument("--rwip-link-probe", action="store_true")
    return parser.parse_args()


def resolve_native_component_sources(
    source_root: Path, definitions: list[str]
) -> list[Path]:
    sources: list[Path] = []
    seen: set[Path] = set()
    for definition in definitions:
        name, separator, value = definition.partition("=")
        if not separator or not COMPONENT_NAME.fullmatch(name):
            raise RunnerError(f"invalid native component source: {definition}")
        source = resolve_under(source_root, value, f"native component {name} source")
        if source.suffix not in (".c", ".cc", ".cpp", ".cxx", ".s", ".S"):
            raise RunnerError(f"unsupported native component source: {source}")
        if not source.is_file():
            raise RunnerError(f"native component source is missing: {source}")
        if source in seen:
            raise RunnerError(f"native source has multiple owners: {source}")
        seen.add(source)
        sources.append(source)
    return sorted(sources)


def resolve_native_include_roots(
    source_root: Path, definitions: list[str]
) -> list[Path]:
    include_roots: set[Path] = set()
    for definition in definitions:
        include_root = resolve_under(
            source_root, definition, "native component include root"
        )
        if not include_root.is_dir():
            raise RunnerError(f"native component include root is missing: {include_root}")
        include_roots.add(include_root)
    return sorted(include_roots)


def render_native_component_manifest(
    sources: list[Path],
    include_roots: list[Path],
    prebuilt_components: dict[str, Path],
) -> str:
    lines = [
        "# Generated from the Bazel native component graph.",
        "H2_BAZEL_NATIVE_SRCS := "
        + (" " + chr(92) + "\n").join(str(source) for source in sources),
        "H2_BAZEL_NATIVE_INCLUDES := "
        + " ".join(f"-I{include_root}" for include_root in include_roots),
        "H2_BAZEL_ARCHIVES := "
        + " ".join(
            str(prebuilt_components[name])
            for name in sorted(prebuilt_components)
        ),
    ]
    lines.extend(
        f"H2_BAZEL_PREBUILT_{name.upper()} := {prebuilt_components[name]}"
        for name in sorted(prebuilt_components)
    )
    return "\n".join(lines) + "\n"


def required_environment(arguments: argparse.Namespace) -> dict[str, str]:
    sdk_locator = read_locator(arguments.sdk_locator, "bk3633-sdk")
    toolchain_locator = read_locator(arguments.toolchain_locator, "bk-arm-toolchain")
    sdk = locator_path(sdk_locator, "root", "bk3633-sdk")
    toolchain = locator_path(toolchain_locator, "bin", "bk-arm-toolchain")
    environment = fixed_environment([toolchain])
    environment.update({
        "BK3633_PATH": str(sdk),
        "COMPILER_TOOLCHAIN_PATH": str(toolchain),
        "H2_GIZOS_ROOT": str(GIZOS_ROOT),
        "H2_NATIVE_BUILD_JOBS": "4",
    })
    return environment


def resolve_prebuilt_components(
    source_root: Path, definitions: list[str]
) -> dict[str, Path]:
    components: dict[str, Path] = {}
    for definition in definitions:
        name, separator, value = definition.partition("=")
        if not separator or not COMPONENT_NAME.fullmatch(name):
            raise RunnerError(f"invalid prebuilt component definition: {definition}")
        if name in components:
            raise RunnerError(f"duplicate prebuilt component: {name}")
        archive = resolve_under(
            source_root, value, f"prebuilt component {name} archive"
        )
        if not archive.is_file() or archive.suffix != ".a":
            raise RunnerError(
                f"BK3633 prebuilt component {name} archive is invalid: {archive}"
            )
        components[name] = archive
    if "h2_firmware_lib" not in components:
        raise RunnerError(
            "required prebuilt component is missing: h2_firmware_lib"
        )
    return components


def executable(path: Path, label: str) -> Path:
    resolved = path.resolve()
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise RunnerError(f"{label} is not executable: {path}")
    return resolved


def command_path(name: str, environment: dict[str, str]) -> Path:
    value = shutil.which(name, path=environment["PATH"])
    if not value:
        raise RunnerError(f"required command is unavailable: {name}")
    return executable(Path(value), name)


def run(
    command: list[str], environment: dict[str, str], cwd: Path | None = None
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        env=environment,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def git_output(
    git: Path, checkout: Path, arguments: list[str], environment: dict[str, str]
) -> str:
    result = run([str(git), "-C", str(checkout), *arguments], environment)
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RunnerError(f"git {' '.join(arguments)} failed: {detail}")
    return result.stdout.strip()


def checkout_state(git: Path, checkout: Path, environment: dict[str, str]) -> str:
    return git_output(
        git,
        checkout,
        ["status", "--porcelain=v1", "--untracked-files=all", "--ignored=matching"],
        environment,
    )


def validate_sdk(
    git: Path,
    checkout: Path,
    expected_commit: str,
    environment: dict[str, str],
) -> None:
    root = Path(
        git_output(git, checkout, ["rev-parse", "--show-toplevel"], environment)
    ).resolve()
    if root != checkout:
        raise RunnerError(f"BK3633_PATH must point to the Git root: {checkout}")
    commit = git_output(git, checkout, ["rev-parse", "HEAD"], environment)
    if commit != expected_commit:
        raise RunnerError(
            f"BK3633 SDK commit mismatch: expected {expected_commit}, found {commit}"
        )
    required = (
        "SDK/src/system/BK3633_STACK_ALLROLES.elf",
        "SDK/projects/app_gatt_all_roles/output/stack/BK3633_STACK_ALLROLES.bin",
        "SDK/projects/boot_for_all_roles/Makefile",
    )
    for relative in required:
        if not (checkout / relative).is_file():
            raise RunnerError(f"required BK3633 SDK input is missing: {relative}")


def validate_toolchain(
    toolchain: Path,
    expected_version: str,
    environment: dict[str, str],
) -> None:
    compiler = executable(toolchain / "arm-none-eabi-gcc", "arm-none-eabi-gcc")
    version_result = run(
        [str(compiler), "-dumpfullversion", "-dumpversion"], environment
    )
    version = version_result.stdout.strip()
    if version_result.returncode != 0 or version != expected_version:
        raise RunnerError(
            f"BK ARM toolchain mismatch: expected {expected_version}, found {version}"
        )
    specs = run([str(compiler), "-print-file-name=nosys.specs"], environment)
    specs_path = specs.stdout.strip()
    if specs.returncode != 0 or specs_path == "nosys.specs" or not Path(specs_path).is_file():
        raise RunnerError("ARM toolchain does not provide nosys.specs")


def resolve_under(root: Path, value: str, label: str) -> Path:
    path = Path(value)
    if path.is_absolute() or ".." in path.parts:
        raise RunnerError(f"{label} escapes the source root: {value}")
    return (root / path).resolve()


def release_file(root: Path, name: str, label: str) -> Path:
    root = root.resolve()
    path = root / name
    if path.is_symlink():
        raise RunnerError(f"{label} must not be a symlink: {path}")
    try:
        mode = path.stat().st_mode
    except FileNotFoundError as error:
        raise RunnerError(f"{label} is missing: {path}") from error
    if not stat.S_ISREG(mode) or path.stat().st_size == 0:
        raise RunnerError(f"{label} must be a non-empty regular file: {path}")
    resolved = path.resolve()
    if root != resolved.parent:
        raise RunnerError(f"{label} escapes the release directory: {path}")
    return resolved


def native_manifest(
    release_root: Path, arguments: argparse.Namespace
) -> tuple[dict[str, object], dict[str, Path]]:
    manifest_path = release_file(release_root, "manifest.json", "native manifest")
    try:
        data = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RunnerError(f"native manifest is invalid JSON: {error}") from error
    if not isinstance(data, dict):
        raise RunnerError("native manifest must be a JSON object")
    expected = {
        "target": "bk3633",
        "board": arguments.board,
        "image": arguments.image,
        "stack": "allroles",
        "elf": f"{arguments.native_target}.elf",
        "bin": f"{arguments.native_target}.bin",
        "map": f"{arguments.native_target}.map",
        "merge": arguments.native_merge,
        "runtime": "baremetal_ringbuf",
        "toolchain": f"arm-none-eabi-gcc {arguments.expected_toolchain_version}",
        "binconverter": "portable-c",
        "merge_inputs": ["bim.bin", "stack.bin", "app.bin"],
    }
    for key, value in expected.items():
        if data.get(key) != value:
            raise RunnerError(
                f"native manifest {key} mismatch: expected {value!r}, found {data.get(key)!r}"
            )
    if not isinstance(data.get("persistent_layout"), dict):
        raise RunnerError("native manifest persistent_layout must be a JSON object")
    files = {
        "elf": release_file(release_root, expected["elf"], "application ELF"),
        "bin": release_file(release_root, expected["bin"], "application BIN"),
        "map": release_file(release_root, expected["map"], "linker map"),
        "merge": release_file(release_root, expected["merge"], "merge CRC image"),
    }
    return data, files


def copy_output(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


def run_native_make(
    make: Path,
    project: Path,
    environment: dict[str, str],
    variables: list[str],
    targets: list[str],
    stage: str,
) -> None:
    result = subprocess.run(
        [
            str(make),
            f"-j{environment['H2_NATIVE_BUILD_JOBS']}",
            "-C",
            str(project.parent),
            *variables,
            *targets,
        ],
        env=environment,
        check=False,
    )
    if result.returncode != 0:
        raise RunnerError(
            f"native BK3633 {stage} failed with exit code {result.returncode}"
        )


def build(arguments: argparse.Namespace) -> None:
    try:
        environment = required_environment(arguments)
    except NativeRuntimeError as error:
        raise RunnerError(str(error)) from error
    source_root = Path(arguments.source_root).resolve()
    prebuilt_components = resolve_prebuilt_components(
        source_root, arguments.prebuilt_component
    )
    native_component_sources = resolve_native_component_sources(
        source_root, arguments.native_component_source
    )
    native_include_roots = resolve_native_include_roots(
        source_root, arguments.native_include_root
    )
    project = resolve_under(source_root, arguments.project, "project Makefile")
    if project.name != "Makefile" or not project.is_file():
        raise RunnerError(f"project must name an existing Makefile: {project}")
    sdk = Path(environment["BK3633_PATH"]).resolve()
    toolchain = Path(environment["COMPILER_TOOLCHAIN_PATH"]).resolve()
    git = command_path("git", environment)
    make = command_path("make", environment)
    binconverter_path = Path(arguments.binconverter)
    if not binconverter_path.is_absolute():
        binconverter_path = source_root / binconverter_path
    binconverter = executable(binconverter_path, "BinConvert")
    expected_sdk_commit = read_expected_commit(
        arguments.sdk_version_file,
        "BK3633 SDK",
    )
    if not Path(arguments.toolchain_archives_file).is_file():
        raise RunnerError("BK ARM toolchain archive identity is missing")
    validate_sdk(git, sdk, expected_sdk_commit, environment)
    validate_toolchain(toolchain, arguments.expected_toolchain_version, environment)
    before = checkout_state(git, sdk, environment)
    if before:
        raise RunnerError(f"BK3633 checkout must be clean before build:\n{before}")

    failure: BaseException | None = None
    try:
        with tempfile.TemporaryDirectory(prefix="h2-bk3633-") as temporary:
            temporary_root = Path(temporary).resolve()
            build_root = temporary_root / "build"
            release_root = temporary_root / "release"
            build_root.mkdir()
            release_root.mkdir()
            native_environment = dict(environment)
            native_toolchain = toolchain
            try:
                ccache = configure_ccache_environment(
                    native_environment,
                    "bk3633",
                    temporary_root,
                    arguments.ccache_runtime_locator,
                )
                if ccache is not None:
                    native_toolchain = create_wrapped_toolchain(
                        toolchain,
                        temporary_root / "ccache-toolchain",
                        ccache,
                    )
            except NativeCcacheError as error:
                raise RunnerError(str(error)) from error
            native_environment["COMPILER_TOOLCHAIN_PATH"] = str(native_toolchain)
            component_manifest = temporary_root / "h2_bazel_components.mk"
            # The project Makefile includes this manifest before sdk_env_check,
            # so H2_BAZEL_ARCHIVES is a Make variable rather than a process
            # environment variable.
            component_manifest.write_text(
                render_native_component_manifest(
                    native_component_sources,
                    native_include_roots,
                    prebuilt_components,
                ),
                encoding="utf-8",
            )
            variables = [
                f"BK3633_PATH={sdk}",
                f"COMPILER_TOOLCHAIN_PATH={native_toolchain}",
                f"H2_FIRMWARE_VERSION={arguments.version}",
                f"RELEASE_DIR={release_root}",
                f"BINCONVERT={binconverter}",
                f"H2_BAZEL_COMPONENT_MANIFEST={component_manifest}",
            ]
            run_native_make(
                make,
                project,
                native_environment,
                [*variables, f"BUILD_DIR={build_root}"],
                ["sdk_probe"],
                "SDK probe",
            )
            if arguments.rwip_link_probe:
                run_native_make(
                    make,
                    project,
                    native_environment,
                    [*variables, f"BUILD_DIR={temporary_root / 'rwip-link-probe'}"],
                    ["rwip_link_probe"],
                    "RWIP link probe",
                )
            run_native_make(
                make,
                project,
                native_environment,
                [*variables, f"BUILD_DIR={build_root}"],
                ["release"],
                "release build",
            )
            data, files = native_manifest(release_root, arguments)
            canonical = {
                key: data[key]
                for key in (
                    "target",
                    "board",
                    "image",
                    "stack",
                    "runtime",
                    "toolchain",
                    "binconverter",
                    "persistent_layout",
                    "merge_inputs",
                )
            }
            canonical.update(
                {
                    "entry": arguments.entry,
                    "version": arguments.version,
                    "elf": "firmware.elf",
                    "bin": "app.bin",
                    "map": "firmware.map",
                    "merge": "merge-crc.bin",
                }
            )
            destinations = {
                "elf": Path(arguments.elf_output),
                "bin": Path(arguments.app_output),
                "map": Path(arguments.map_output),
                "merge": Path(arguments.recovery_output),
            }
            for key, source in files.items():
                copy_output(source, destinations[key])
            manifest_output = Path(arguments.manifest_output)
            manifest_output.parent.mkdir(parents=True, exist_ok=True)
            manifest_output.write_text(
                json.dumps(canonical, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
    except BaseException as error:
        failure = error
    after = checkout_state(git, sdk, environment)
    if after != before:
        raise RunnerError(
            "BK3633 checkout changed during native build\n"
            f"before:\n{before}\nafter:\n{after}"
        ) from failure
    if failure is not None:
        raise failure


def main() -> int:
    try:
        build(parse_arguments())
    except RunnerError as error:
        print(f"error: {error}", file=os.sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
