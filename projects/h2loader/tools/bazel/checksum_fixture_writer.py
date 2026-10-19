"""Build controlled old/new Loader checksum E2E packages from two raw Apps."""
from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile

sys.path.insert(0, str(Path(__file__).resolve().parents[4]))
from projects.h2loader.tools.bazel.firmware_artifacts import BundleEntry, bundle_checksum, write_package

CASES = ("baseline", "unchanged", "app-only", "data-only", "both-changed")


def guard_skipped_streams(path: Path, app: bool, data: bool) -> None:
    packet = bytearray(path.read_bytes())
    with tarfile.open(fileobj=io.BytesIO(packet), mode="r:") as tar:
        manifest = tar.getmember("manifest")
        text = tar.extractfile(manifest).read()
        for name, guarded in (("app.bin.zlib", app), ("data.tar.zlib", data)):
            if not guarded:
                continue
            member = tar.getmember(name)
            packet[member.offset_data] = 0  # Invalid CMF; no valid inflater can accept it.
            digest = hashlib.sha256(packet[member.offset_data:member.offset_data + member.size]).hexdigest()
            key = b"app_zlib_sha256=" if app and name == "app.bin.zlib" else b"data_zlib_sha256="
            start = text.index(key) + len(key)
            packet[manifest.offset_data + start:manifest.offset_data + start + 64] = digest.encode("ascii")
    path.write_bytes(packet)


def write_matrix(output: Path, app_a: bytes, app_b: bytes, *, app_path: str,
                 board: str, target: str, version_a: str, version_b: str) -> None:
    if not app_a or not app_b or app_a == app_b:
        raise ValueError("checksum matrix requires two different bootable App images")
    datasets = [[BundleEntry("data/h2loader-e2e/probe.txt", b"h2loader-e2e-data-a\n")],
                [BundleEntry("data/h2loader-e2e/probe.txt", b"h2loader-e2e-data-b\n")]]
    combinations = ((0, 0), (0, 0), (1, 0), (1, 1), (0, 0))
    receipt = {"schema": "h2loader-checksum-fixtures/v1", "board": board, "target": target, "packages": []}
    for package_format, family in ((1, "tar_zlib"), (2, "zlib_tar")):
        suffix = ".update.tar.zlib" if package_format == 1 else ".update.tar"
        for index, (app_index, data_index) in enumerate(combinations):
            app = (app_a, app_b)[app_index]
            version = (version_a, version_b)[app_index]
            path = output / family / (CASES[index] + suffix)
            write_package(path, app_path, app, datasets[data_index], role="app", board=board,
                          target=target, version=version, package_format=package_format)
            guard_app = package_format == 2 and index in (1, 3)
            guard_data = package_format == 2 and index in (1, 2)
            if guard_app or guard_data:
                guard_skipped_streams(path, guard_app, guard_data)
            receipt["packages"].append({"format": package_format, "case": CASES[index],
                "path": path.relative_to(output).as_posix(), "bytes": path.stat().st_size,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "image_sha256": hashlib.sha256(app).hexdigest(),
                "data_sha256": bundle_checksum(datasets[data_index]),
                "guarded_app": guard_app, "guarded_data": guard_data})
    (output / "fixtures.json").write_text(json.dumps(receipt, indent=2) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    for name in ("app-a", "app-b", "app-path", "board", "target", "version-a", "version-b", "output"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    write_matrix(Path(args.output), Path(args.app_a).read_bytes(), Path(args.app_b).read_bytes(),
                 app_path=args.app_path, board=args.board, target=args.target,
                 version_a=args.version_a, version_b=args.version_b)

if __name__ == "__main__":
    main()
