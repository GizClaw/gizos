"""Consume only the built npm tarball, including its real ESM/type exports."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

parser = argparse.ArgumentParser()
for name in ("directory", "manifest", "node", "compiler", "consumer", "runtime_consumer"):
    parser.add_argument("--" + name, type=Path, required=True)
args = parser.parse_args()
manifest = json.loads(args.manifest.read_text())
name = manifest["name"].removeprefix("@").replace("/", "-") + "-" + manifest["version"] + ".tgz"
tarball = args.directory / name
assert list(args.directory.iterdir()) == [tarball]
expected = {"package/" + name for name in [*manifest["files"], "package.json"]}
assert tarball.read_bytes()[:8] == b"\x1f\x8b\x08\x00\x00\x00\x00\x00"
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    with tarfile.open(tarball) as archive:
        members = archive.getmembers()
        assert {item.name for item in members if item.isfile()} == expected
        assert [item.name for item in members] == sorted({item.name for item in members})
        for item in members:
            assert item.isdir() or item.isfile()
            assert (item.mtime, item.uid, item.gid, item.uname, item.gname) == (0, 0, 0, "", "")
            assert item.mode == (0o755 if item.isdir() else 0o644)
            assert not item.pax_headers
        archive.extractall(root, filter="data")
    package = root / "node_modules/@gizclaw/firmware"
    package.parent.mkdir(parents=True)
    (root / "package").rename(package)
    assert json.loads((package / "package.json").read_text()) == manifest
    (root / "package.json").write_text(json.dumps({"private": True, "type": "module"}))
    subprocess.run([
        str(args.node.resolve()), "--input-type=module", "--eval",
        "import {createFirmwareClient} from '@gizclaw/firmware'; "
        "if (typeof createFirmwareClient !== 'function' || globalThis.h2WebGizClaw !== undefined) process.exit(1); "
        "console.log('GIZCLAW_FIRMWARE_NPM import=PASS');",
    ], cwd=root, check=True)
    shutil.copyfile(args.runtime_consumer, root / "runtime_consumer.mjs")
    subprocess.run([str(args.node.resolve()), str(root / "runtime_consumer.mjs")], cwd=root, check=True)
    shutil.copyfile(args.consumer, root / "consumer.ts")
    (root / "tsconfig.json").write_text(json.dumps({
        "compilerOptions": {"strict": True, "noEmit": True, "target": "ES2022",
                            "module": "NodeNext", "moduleResolution": "NodeNext",
                            "lib": ["ES2022", "DOM"], "types": []},
        "files": ["consumer.ts"],
    }))
    subprocess.run([str(args.node.resolve()), str(args.compiler.resolve()),
                    "-p", str(root / "tsconfig.json")], check=True)
    print("GIZCLAW_FIRMWARE_NPM types=PASS")
