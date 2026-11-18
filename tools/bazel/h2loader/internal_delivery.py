"""Validate a Fruit Slice AC791N build and deliver its three files privately.

This manual delivery uses the existing private cache bucket, outside the Bazel
cache namespace. It never creates public URLs, Release assets or device jobs.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import urllib.parse
import urllib.request

# Direct invocation from a preserved workflow script still loads the selected
# build source's package validator. Bazel supplies the same module via deps.
sys.path.insert(0, str(Path.cwd()))
from tools.bazel.h2loader.firmware_artifacts import read_managed_package


BUCKET = "gizclaw-remote-bazel-cache"
STEM = "jieli_ac791n_devkit-fruit-slice-wl82"
TARGET = "//projects/fruit_slice/targets/h2_loader_tar_zlib/jieli/jieli_ac791n_devkit:package"
IDENTITY = {"board": "jieli_ac791n_devkit", "target": "wl82", "role": "app"}


def validate_compilation_mode(mode: str, receipt: dict | None = None) -> str:
    if mode not in ("fastbuild", "opt"):
        raise ValueError("unsupported delivery compilation mode")
    if receipt is None:
        if mode != "fastbuild":
            raise ValueError("opt delivery requires a configured-target mode receipt")
    elif receipt != {"requested": mode, "observed": mode}:
        raise ValueError("configured target compilation mode mismatch")
    return mode


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def validate_package(package: bytes, metadata: bytes, version: str) -> dict:
    parsed = read_managed_package(package)
    info = json.loads(metadata)
    expected = {**IDENTITY, "version": version}
    for key, value in expected.items():
        if parsed.manifest[key] != value or info[key] != value:
            raise ValueError(f"package/metadata identity mismatch: {key}")
    if parsed.manifest["format"] != "1" or info["package_format"] != 1:
        raise ValueError("expected format 1")
    if parsed.app_path != "app/jieli/update.ufw" or info["image"] != "fruit-slice":
        raise ValueError("unexpected App image")
    for key, value in parsed.manifest.items():
        if str(info["package_manifest"][key]) != value:
            raise ValueError(f"metadata manifest mismatch: {key}")
    assets = info["assets"]
    if len(assets) != 1 or assets[0]["operation"] != "managed-install":
        raise ValueError("expected one managed-install asset")
    asset = assets[0]
    if (asset["name"] != STEM + ".update.tar.zlib"
            or asset["size"] != len(package) or asset["sha256"] != digest(package)):
        raise ValueError("metadata package name/size/SHA-256 mismatch")
    return parsed.manifest


def upload(name: str, data: bytes, token: str, content_type: str) -> None:
    # Create-only object writes prevent replacing a previous run's evidence.
    query = urllib.parse.urlencode({
        "uploadType": "media", "name": name, "ifGenerationMatch": "0",
    })
    request = urllib.request.Request(
        f"https://storage.googleapis.com/upload/storage/v1/b/{BUCKET}/o?{query}",
        data=data, method="POST", headers={
            "Authorization": "Bearer " + token, "Content-Type": content_type,
        },
    )
    with urllib.request.urlopen(request, timeout=120) as response:
        result = json.load(response)
    if result["bucket"] != BUCKET or result["name"] != name:
        raise ValueError("storage receipt identity mismatch")
    if int(result["size"]) != len(data):
        raise ValueError("storage receipt size mismatch")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--outputs", type=Path, required=True)
    parser.add_argument("--mode-receipt", type=Path)
    args = parser.parse_args()
    mode_receipt = json.loads(args.mode_receipt.read_text()) if args.mode_receipt else None
    mode = validate_compilation_mode(os.environ.get("DELIVERY_COMPILATION_MODE", "fastbuild"), mode_receipt)
    source = os.environ["SOURCE_SHA"]
    workflow = os.environ["WORKFLOW_SHA"]
    for value in (source, workflow):
        if re.fullmatch(r"[0-9a-f]{40}", value) is None:
            raise ValueError("source and workflow must be full commit SHAs")
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    if actual != source:
        raise ValueError("source checkout mismatch")
    changed = subprocess.check_output(["git", "diff", "--name-only", "HEAD"], text=True).strip()
    if changed:
        raise ValueError("tracked source changed during build: " + changed)
    subprocess.run(["git", "diff", "--quiet", "HEAD"], check=True)
    run, attempt = os.environ["GITHUB_RUN_ID"], os.environ["GITHUB_RUN_ATTEMPT"]
    if not run.isdecimal() or not attempt.isdecimal():
        raise ValueError("invalid run identity")
    version = os.environ["EXPECTED_VERSION"]
    expected_names = {STEM + ".update.tar.zlib", STEM + ".firmware.json"}
    paths = [Path(line) for line in args.outputs.read_text().splitlines() if line]
    if len(paths) != 2 or {path.name for path in paths} != expected_names:
        raise ValueError("Bazel must return exactly the declared package and metadata")
    files = {path.name: path.read_bytes() for path in paths}
    manifest = validate_package(files[STEM + ".update.tar.zlib"],
                                files[STEM + ".firmware.json"], version)
    prefix = f"internal-delivery/fruit-slice/ac791n/{source}/{run}/{attempt}"
    record = {
        "source_sha": source, "workflow_sha": workflow,
        "run_url": f"https://github.com/{os.environ['GITHUB_REPOSITORY']}/actions/runs/{run}",
        "run_attempt": attempt, "created_utc": datetime.now(timezone.utc).isoformat(),
        "target": TARGET, "config": "ac791n", "compilation_mode": mode,
        "compilation_mode_receipt": mode_receipt,
        "remote_download_outputs": "all", "package_manifest": manifest,
        "files": [{"name": name, "size": len(data), "sha256": digest(data)}
                  for name, data in sorted(files.items())],
    }
    files["build-record.json"] = (json.dumps(record, indent=2, sort_keys=True) + "\n").encode()
    token = os.environ["DELIVERY_TOKEN"]
    if not token:
        raise ValueError("missing private delivery authorization")
    for name, data in sorted(files.items()):
        upload(prefix + "/" + name, data, token,
               "application/json" if name.endswith(".json") else "application/octet-stream")
    # Object location is not a public or signed URL and contains no credential.
    print(f"Validated private delivery: gs://{BUCKET}/{prefix}/")
    print(f"Source {source}, version {version}; package and metadata checks passed")


if __name__ == "__main__":
    main()
