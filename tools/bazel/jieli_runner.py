#!/usr/bin/env python3
"""Run one JieLi SDK native build plus local post-build for a Bazel action.

The SDK Makefiles write every intermediate (generated linker script, ELF,
`download.sh`) into the SDK tree. The runner therefore copies the pinned SDK
checkout into an invocation-local tree, drives the repository-owned project
selected by the board layout, and runs the repository-owned local post-build
script that replaces the cloud packager with the Linux
`isd_download`/`fw_add`/`ufw_maker` tools. The private SDK remains a source and
library substrate; its application Makefiles are never selected or included.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import re
import resource
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from native_runtime import (  # noqa: E402
    NativeRuntimeError,
    fixed_environment,
    locator_path,
    read_locator,
)

COMMIT = re.compile(r"[0-9a-f]{40}")
SAFE_NAME = re.compile(r"[A-Za-z0-9_.-]+")
FAMILIES = {"br23": "ac695n", "wl82": "ac791n"}
IGNORED_SDK_DIRECTORIES = (".git", "doc", "ui_project")
OUTPUT_NAMES = {
    "elf": "firmware.elf",
    "symbols": "symbols.txt",
    "flash_image": "jl_isd.bin",
    "fw": "jl_isd.fw",
    "update": "update.ufw",
}
REQUIRED_OPEN_FILES = 8192


class RunnerError(RuntimeError):
    """The native JieLi build cannot be completed as declared."""


def read_expected_commit(path: str, label: str) -> str:
    try:
        commit = Path(path).read_text(encoding="utf-8").strip()
    except OSError as error:
        raise RunnerError(f"cannot read {label} commit file: {error}") from error
    if not COMMIT.fullmatch(commit):
        raise RunnerError(f"invalid expected {label} commit: {commit}")
    return commit


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--target", required=True, choices=sorted(FAMILIES))
    parser.add_argument("--entry", required=True)
    parser.add_argument("--board", required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--project-makefile", required=True)
    parser.add_argument("--project-rules", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--sdk-version-file", required=True)
    parser.add_argument("--toolchain-archives-file", required=True)
    parser.add_argument("--sdk-locator", required=True)
    parser.add_argument("--toolchain-locator", required=True)
    parser.add_argument("--postbuild-locator", required=True)
    parser.add_argument("--post-script", required=True)
    parser.add_argument("--elf-output", required=True)
    parser.add_argument("--symbols-output", required=True)
    parser.add_argument("--flash-image-output", required=True)
    parser.add_argument("--fw-output", required=True)
    parser.add_argument("--update-output", required=True)
    parser.add_argument("--manifest-output", required=True)
    parser.add_argument("--native-component", action="append", default=[])
    parser.add_argument("--native-component-source", action="append", default=[])
    parser.add_argument("--native-include-root", action="append", default=[])
    parser.add_argument("--prebuilt-component", action="append", default=[])
    parser.add_argument("--sdk-patch", action="append", default=[])
    return parser.parse_args(argv)


def required_environment(arguments: argparse.Namespace) -> dict[str, str]:
    family = FAMILIES[arguments.target]
    sdk_locator = read_locator(arguments.sdk_locator, f"jieli-{family}-sdk")
    toolchain_locator = read_locator(arguments.toolchain_locator, "jieli-toolchain")
    postbuild_locator = read_locator(arguments.postbuild_locator, "jieli-postbuild")
    sdk = locator_path(sdk_locator, "root", f"jieli-{family}-sdk")
    paths = sdk_locator["paths"]
    checkout = Path(paths["checkout"]) if isinstance(paths, dict) and "checkout" in paths else sdk
    toolchain = locator_path(toolchain_locator, "root", "jieli-toolchain")
    toolchain_bin = locator_path(toolchain_locator, "pi32v2_bin", "jieli-toolchain")
    postbuild = locator_path(postbuild_locator, "root", "jieli-postbuild")
    environment = fixed_environment([toolchain_bin])
    environment.update(
        {
            "H2_NATIVE_BUILD_JOBS": "4",
            "JIELI_POSTBUILD_ROOT": str(postbuild),
            "JIELI_SDK_CHECKOUT": str(checkout),
            "JIELI_SDK_ROOT": str(sdk),
            "JIELI_TOOLCHAIN_BIN": str(toolchain_bin),
            "JIELI_TOOLCHAIN_ROOT": str(toolchain),
            # The post-build tools are Qt programs; never open a display.
            "QT_QPA_PLATFORM": "offscreen",
        }
    )
    return environment


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


def git_output(
    git: Path, checkout: Path, environment: dict[str, str], *arguments: str
) -> str:
    result = subprocess.run(
        [str(git), "-C", str(checkout), *arguments],
        env=environment,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        raise RunnerError(
            f"git {' '.join(arguments)} failed for {checkout}: {result.stderr.strip()}"
        )
    return result.stdout.strip()


def checkout_state(git: Path, checkout: Path, environment: dict[str, str]) -> str:
    return git_output(
        git, checkout, environment, "status", "--porcelain=v1", "--untracked-files=no"
    )


def validate_sdk(
    git: Path, sdk: Path, expected_commit: str, environment: dict[str, str]
) -> None:
    toplevel = Path(git_output(git, sdk, environment, "rev-parse", "--show-toplevel"))
    if toplevel.resolve() != sdk.resolve():
        raise RunnerError(f"JieLi SDK locator must name the Git root: {sdk}")
    actual = git_output(git, sdk, environment, "rev-parse", "HEAD")
    if actual != expected_commit:
        raise RunnerError(
            f"JieLi SDK commit mismatch: expected {expected_commit}, found {actual}"
        )


def validate_toolchain(environment: dict[str, str]) -> Path:
    toolchain_bin = Path(environment["JIELI_TOOLCHAIN_BIN"])
    for name in ("clang", "lto-wrapper", "objcopy", "objdump", "objsizedump"):
        executable(toolchain_bin / name, f"JieLi toolchain {name}")
    postbuild = Path(environment["JIELI_POSTBUILD_ROOT"])
    for name in ("isd_download", "fw_add", "ufw_maker", "remove_tailing_zeros"):
        executable(postbuild / name, f"JieLi post-build {name}")
    return toolchain_bin


def require_supported_host() -> None:
    """Fail closed before touching the SDK when the host cannot run the tools.

    JieLi only publishes x86_64 Linux builds of clang, lto-wrapper and the Qt
    post-build programs; Bazel compatibility already skips other hosts, and
    this check keeps a direct runner invocation equally explicit.
    """
    system = platform.system()
    machine = platform.machine().lower()
    if system != "Linux" or machine not in ("x86_64", "amd64"):
        raise RunnerError(
            "UNSUPPORTED: the JieLi toolchain ships Linux x86_64 binaries only; "
            f"host is {system} {machine}. Run the build on a Linux x86_64 host or "
            "inside the Linux dev container."
        )


def raise_open_file_limit() -> None:
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    if soft >= REQUIRED_OPEN_FILES:
        return
    wanted = REQUIRED_OPEN_FILES
    if hard != resource.RLIM_INFINITY and hard < wanted:
        wanted = hard
    try:
        resource.setrlimit(resource.RLIMIT_NOFILE, (wanted, hard))
    except (OSError, ValueError) as error:
        raise RunnerError(
            f"cannot raise the open file limit required by the JieLi linker: {error}"
        ) from error
    if wanted < REQUIRED_OPEN_FILES:
        raise RunnerError(
            "the JieLi LTO linker requires at least "
            f"{REQUIRED_OPEN_FILES} open files; hard limit is {hard}"
        )


def copy_sdk(source: Path, destination: Path) -> None:
    """Copy the SDK root, skipping only its top-level VCS/doc directories.

    Nested directories that happen to share a name (for example a component's
    own `doc/`) stay in the copy because the SDK Makefiles may reference them.
    """
    source = source.resolve()

    def ignore(directory: str, names: list[str]) -> set[str]:
        if Path(directory).resolve() != source:
            return set()
        return {name for name in names if name in IGNORED_SDK_DIRECTORIES}

    shutil.copytree(source, destination, symlinks=True, ignore=ignore)


def apply_sdk_patches(
    git: Path,
    sdk_copy: Path,
    patches: list[Path],
    environment: dict[str, str],
) -> None:
    """Apply declared repository patches only to the invocation-local SDK."""
    for patch in patches:
        result = subprocess.run(
            [str(git), "-C", str(sdk_copy), "apply", "--whitespace=error", str(patch)],
            env=environment,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if result.returncode != 0:
            raise RunnerError(
                f"cannot apply JieLi SDK patch {patch}: {result.stderr.strip()}"
            )


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
            str(project),
            *variables,
            *targets,
        ],
        env=environment,
        check=False,
    )
    if result.returncode != 0:
        raise RunnerError(
            f"native JieLi {stage} failed with exit code {result.returncode}"
        )


def release_file(root: Path, name: str, label: str) -> Path:
    path = root / name
    if path.is_symlink():
        raise RunnerError(f"{label} must not be a symlink: {path}")
    if not path.is_file() or path.stat().st_size == 0:
        raise RunnerError(f"{label} must be a non-empty file: {path}")
    return path


def copy_output(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


def resolve_under(root: Path, value: str, label: str) -> Path:
    """Resolves a source-root-relative input.

    Bazel execroots expose workspace files as symlinks, so the check is on the
    relative path (no absolute paths, no `..`) rather than on the resolved
    location.
    """
    path = Path(value)
    if path.is_absolute() or ".." in path.parts:
        raise RunnerError(f"{label} escapes the source root: {value}")
    return (root / path).resolve()


def resolve_native_sources(root: Path, definitions: list[str]) -> list[Path]:
    owners: dict[Path, str] = {}
    for definition in definitions:
        name, separator, value = definition.partition("=")
        if not separator or not SAFE_NAME.fullmatch(name) or not value:
            raise RunnerError(f"invalid native component source: {definition}")
        source = resolve_under(root, value, f"native component {name} source")
        if not source.is_file():
            raise RunnerError(f"native component source is missing: {source}")
        previous = owners.get(source)
        if previous is not None and previous != name:
            raise RunnerError(f"native source has multiple owners: {source}")
        owners[source] = name
    return sorted(owners)


def resolve_include_roots(root: Path, definitions: list[str]) -> list[Path]:
    roots: set[Path] = set()
    for definition in definitions:
        include_root = resolve_under(root, definition, "native include root")
        if not include_root.is_dir():
            raise RunnerError(f"native include root is missing: {include_root}")
        roots.add(include_root)
    return sorted(roots)


def shell_quote_define(name: str, value: str) -> str:
    if not SAFE_NAME.fullmatch(value):
        raise RunnerError(f"define value is not shell safe: {name}={value}")
    return f'-D{name}=\\"{value}\\"'


def render_component_manifest(
    sources: list[Path],
    include_roots: list[Path],
    archives: dict[str, Path],
    defines: list[str],
) -> str:
    lines = [
        "# Generated from the Bazel native component graph.",
        "H2_BAZEL_NATIVE_SRCS := " + (" " + chr(92) + "\n").join(str(source) for source in sources),
        "H2_BAZEL_NATIVE_INCLUDES := " + " ".join(f"-I{root}" for root in include_roots),
        "H2_BAZEL_ARCHIVES := " + " ".join(str(archives[name]) for name in sorted(archives)),
        "H2_BAZEL_DEFINES := " + " ".join(defines),
    ]
    lines.extend(f"H2_BAZEL_PREBUILT_{name.upper()} := {archives[name]}" for name in sorted(archives))
    return "\n".join(lines) + "\n"


def parse_prebuilt_components(definitions: list[str]) -> dict[str, str]:
    components: dict[str, str] = {}
    for definition in definitions:
        name, separator, archive = definition.partition("=")
        if not separator or not SAFE_NAME.fullmatch(name) or not archive:
            raise RunnerError(f"invalid prebuilt component definition: {definition}")
        if name in components:
            raise RunnerError(f"duplicate prebuilt component: {name}")
        components[name] = archive
    return components


def build(arguments: argparse.Namespace) -> None:
    for value, label in (
        (arguments.board, "board"),
        (arguments.image, "image"),
        (arguments.version, "version"),
    ):
        if not SAFE_NAME.fullmatch(value):
            raise RunnerError(f"invalid JieLi {label}: {value}")
    source_root = Path(arguments.source_root).resolve()
    prebuilt_components = parse_prebuilt_components(arguments.prebuilt_component)
    archives = {
        name: resolve_under(source_root, value, f"prebuilt component {name} archive")
        for name, value in prebuilt_components.items()
    }
    for name, archive in archives.items():
        if not archive.is_file() or archive.suffix != ".a":
            raise RunnerError(f"prebuilt component {name} archive is invalid: {archive}")
    native_sources = resolve_native_sources(source_root, arguments.native_component_source)
    include_roots = resolve_include_roots(source_root, arguments.native_include_root)
    project_makefile_label = Path(arguments.project_makefile).as_posix()
    project_makefile = resolve_under(
        source_root, arguments.project_makefile, "JieLi project makefile"
    )
    if not project_makefile.is_file():
        raise RunnerError(f"JieLi project makefile is missing: {project_makefile}")
    project_rules = resolve_under(
        source_root, arguments.project_rules, "JieLi project rules"
    )
    if not project_rules.is_file():
        raise RunnerError(f"JieLi project rules are missing: {project_rules}")
    sdk_patches = [
        resolve_under(source_root, patch, "JieLi SDK patch")
        for patch in arguments.sdk_patch
    ]
    for patch in sdk_patches:
        if not patch.is_file() or patch.suffix != ".patch":
            raise RunnerError(f"JieLi SDK patch is invalid: {patch}")
    require_supported_host()
    try:
        environment = required_environment(arguments)
    except NativeRuntimeError as error:
        raise RunnerError(str(error)) from error
    post_script = Path(arguments.post_script)
    if not post_script.is_absolute():
        post_script = source_root / post_script
    if not post_script.is_file():
        raise RunnerError(f"JieLi post-build script is missing: {post_script}")
    if not Path(arguments.toolchain_archives_file).is_file():
        raise RunnerError("JieLi toolchain archive identity is missing")
    expected_commit = read_expected_commit(arguments.sdk_version_file, "JieLi SDK")
    sdk_checkout = Path(environment["JIELI_SDK_CHECKOUT"]).resolve()
    sdk_root = Path(environment["JIELI_SDK_ROOT"]).resolve()
    if sdk_root != sdk_checkout and sdk_checkout not in sdk_root.parents:
        raise RunnerError(f"SDK root escapes its Git checkout: {sdk_root}")
    git = command_path("git", environment)
    make = command_path("make", environment)
    bash = command_path("bash", environment)
    toolchain_bin = validate_toolchain(environment)
    validate_sdk(git, sdk_checkout, expected_commit, environment)
    before = checkout_state(git, sdk_checkout, environment)
    if before:
        raise RunnerError(
            "JieLi SDK checkout must be clean before build (if the mirror uses "
            "Git LFS, git-lfs must be installed on the build host so LFS files "
            f"do not read as modified):\n{before}"
        )
    raise_open_file_limit()

    failure: BaseException | None = None
    try:
        with tempfile.TemporaryDirectory(prefix=f"h2-jieli-{arguments.target}-") as temporary:
            temporary_root = Path(temporary).resolve()
            sdk_copy = temporary_root / "sdk"
            output_root = temporary_root / "out"
            output_root.mkdir()
            copy_sdk(sdk_root, sdk_copy)
            apply_sdk_patches(git, sdk_copy, sdk_patches, environment)
            native_environment = dict(environment)
            native_environment["HOME"] = str(temporary_root / "home")
            (temporary_root / "home").mkdir()
            native_environment["TMPDIR"] = str(temporary_root / "tmp")
            (temporary_root / "tmp").mkdir()
            manifest_file = temporary_root / "h2_bazel_components.mk"
            manifest_file.write_text(
                render_component_manifest(
                    native_sources,
                    include_roots,
                    archives,
                    [shell_quote_define("H2_JIELI_FIRMWARE_VERSION", arguments.version)],
                ),
                encoding="utf-8",
            )
            variables = [
                f"TOOL_DIR={toolchain_bin}",
                "VERBOSE=0",
                f"H2_BAZEL_COMPONENT_MANIFEST={manifest_file}",
                f"H2_JIELI_PROJECT_RULES={project_rules}",
            ]
            print(
                "JieLi build starting: "
                f"target={arguments.target} "
                f"entry={arguments.entry} "
                f"project={project_makefile_label} "
                f"sdk_commit={expected_commit} "
                f"native_sources={len(native_sources)} "
                f"archives={len(archives)} "
                f"toolchain={toolchain_bin}"
            )
            run_native_make(
                make,
                sdk_copy,
                native_environment,
                ["-f", str(project_makefile), *variables],
                ["h2_link"],
                "link",
            )
            result = subprocess.run(
                [
                    str(bash),
                    str(post_script),
                    str(sdk_copy),
                    environment["JIELI_TOOLCHAIN_ROOT"],
                    environment["JIELI_POSTBUILD_ROOT"],
                    str(output_root),
                ],
                env=native_environment,
                check=False,
            )
            if result.returncode != 0:
                raise RunnerError(
                    f"JieLi local post-build failed with exit code {result.returncode}"
                )
            destinations = {
                "elf": Path(arguments.elf_output),
                "symbols": Path(arguments.symbols_output),
                "flash_image": Path(arguments.flash_image_output),
                "fw": Path(arguments.fw_output),
                "update": Path(arguments.update_output),
            }
            for key, name in OUTPUT_NAMES.items():
                copy_output(release_file(output_root, name, f"JieLi {name}"), destinations[key])
            toolchain_locator = read_locator(arguments.toolchain_locator, "jieli-toolchain")
            postbuild_locator = read_locator(arguments.postbuild_locator, "jieli-postbuild")
            manifest = {
                "board": arguments.board,
                "entry": arguments.entry,
                "family": FAMILIES[arguments.target],
                "image": arguments.image,
                "native_components": sorted(arguments.native_component),
                "native_sources": sorted(
                    definition.partition("=")[2] for definition in arguments.native_component_source
                ),
                "outputs": dict(OUTPUT_NAMES),
                "postbuild": toolchain_identity(postbuild_locator),
                "prebuilt_components": sorted(prebuilt_components),
                "sdk_commit": expected_commit,
                "project_makefile": project_makefile_label,
                "target": arguments.target,
                "toolchain": toolchain_identity(toolchain_locator),
                "version": arguments.version,
            }
            manifest_output = Path(arguments.manifest_output)
            manifest_output.parent.mkdir(parents=True, exist_ok=True)
            manifest_output.write_text(
                json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
    except BaseException as error:  # re-raised after the checkout audit
        failure = error
    after = checkout_state(git, sdk_checkout, environment)
    if after != before:
        raise RunnerError(
            "JieLi SDK checkout changed during native build\n"
            f"before:\n{before}\nafter:\n{after}"
        ) from failure
    validate_sdk(git, sdk_checkout, expected_commit, environment)
    if failure is not None:
        raise failure


def toolchain_identity(locator: dict[str, object]) -> dict[str, str]:
    metadata = locator.get("metadata")
    if not isinstance(metadata, dict):
        return {}
    return {
        key: str(metadata[key])
        for key in ("archive", "archive_sha256", "directory", "tree_sha256")
        if key in metadata
    }


def main(argv: list[str]) -> int:
    try:
        build(parse_arguments(argv))
    except (RunnerError, NativeRuntimeError, OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
