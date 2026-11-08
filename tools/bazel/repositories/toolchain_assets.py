"""Read host-tool pins from existing locked toolchain metadata, without execution."""

import ast
import json
from pathlib import Path, PureWindowsPath
import platform
import tarfile
from urllib.parse import unquote, urlsplit

from github_release import download_verified


_TABLE_LIMIT = 8 * 1024 * 1024


def host_platform():
    system = platform.system().lower()
    system = {"darwin": "darwin", "windows": "windows", "linux": "linux"}.get(system, system)
    machine = platform.machine().lower()
    machine = {"x86_64": "amd64", "amd64": "amd64", "aarch64": "arm64", "arm64": "arm64"}.get(machine, machine)
    return system + "_" + machine


def _version_table(archive):
    with tarfile.open(archive, "r:*") as source:
        candidates = [
            member for member in source.getmembers()
            if member.isfile() and (
                member.name == "yq/toolchain/versions.bzl"
                or member.name.endswith("/yq/toolchain/versions.bzl")
            )
        ]
        if len(candidates) != 1 or candidates[0].size > _TABLE_LIMIT:
            raise ValueError("Locked yq archive must contain one bounded version table")
        with source.extractfile(candidates[0]) as table:
            contents = table.read(_TABLE_LIMIT + 1)
    if len(contents) > _TABLE_LIMIT:
        raise ValueError("Yq version table exceeds size limit")
    # Only a literal data assignment is interpreted; no downloaded code is executed.
    assignments = [
        node.value for node in ast.parse(contents).body
        if isinstance(node, ast.Assign)
        and any(isinstance(target, ast.Name) and target.id == "YQ_VERSIONS" for target in node.targets)
    ]
    if len(assignments) != 1:
        raise ValueError("Yq version table is missing or ambiguous")
    value = ast.literal_eval(assignments[0])
    if not isinstance(value, dict):
        raise ValueError("Yq versions must be a literal dictionary")
    return value


def prefetch_bun(manifest, directory, download=download_verified, host=None):
    manifest = Path(manifest)
    if not manifest.is_file():
        return 0
    pins = json.loads(manifest.read_text())
    system, machine = (host or host_platform()).rsplit("_", 1)
    key = system + "-" + {"amd64": "x64"}.get(machine, machine)
    if key not in pins["archives"]:
        raise ValueError("Unsupported pinned Bun host: " + key)
    asset = pins["archives"][key]
    archive = asset["archive"]
    if (
        not isinstance(archive, str)
        or archive in ("", ".", "..")
        or "/" in archive
        or "\\" in archive
        or PureWindowsPath(archive).name != archive
    ):
        raise ValueError("Bun archive filename must stay inside the distdir")
    name = archive + ".zip"
    url = pins["url"].format(version=pins["version"], archive=archive)
    download(url, asset["sha256"], Path(directory) / name)
    print("Verified Bazel host tool: " + name, flush=True)
    return 1


def prefetch_yq(lock, directory, download=download_verified, host=None):
    host = host or host_platform()
    versions = set()
    for extension in lock.get("moduleExtensions", {}).values():
        for result in extension.values():
            for spec in result.get("generatedRepoSpecs", {}).values():
                rule = spec.get("repoRuleId", "")
                attributes = spec.get("attributes", {})
                if (
                    rule.startswith("@@yq.bzl+")
                    and rule.endswith("%yq_platform_repo")
                    and attributes.get("platform") == host
                ):
                    versions.add(attributes["version"])
    if not versions:
        return 0
    if len(versions) != 1:
        raise ValueError("Multiple locked yq versions collide on the host tool filename")
    metadata = [
        digest for url, digest in lock["registryFileHashes"].items()
        if "/modules/yq.bzl/" in url and url.endswith("/source.json") and digest is not None
    ]
    if len(metadata) != 1:
        raise ValueError("Locked yq registry metadata is missing or ambiguous")
    directory = Path(directory)
    source = json.loads((directory / ".registry" / (metadata[0] + ".json")).read_text())
    filename = unquote(urlsplit(source["url"]).path.rsplit("/", 1)[-1])
    table = _version_table(directory / filename)
    version = next(iter(versions))
    if version not in table or host not in table[version]:
        raise ValueError("Locked yq version has no checksum for this host")
    name = "yq_" + host + (".exe" if host.startswith("windows_") else "")
    url = "https://github.com/mikefarah/yq/releases/download/v" + version + "/" + name
    download(url, table[version][host], directory / name)
    print("Verified Bazel host tool: " + name, flush=True)
    return 1
