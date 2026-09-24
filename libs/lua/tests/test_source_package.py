"""Extract and compile using only manifest.json, a host C compiler and a harness.

Standalone: python3 test_source_package.py PACKAGE.tar.gz [test_embedder.c] [runtime_sources.content_id]
No Bazel invocation or repository source discovery occurs in this test.
"""

import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tarfile
import tempfile


def main():
    package = Path(sys.argv[1]).resolve()
    harness = Path(sys.argv[2] if len(sys.argv) > 2 else
                   Path(__file__).with_name("test_embedder.c")).resolve()
    content_id = Path(sys.argv[3]).resolve() if len(sys.argv) > 3 else None
    with tempfile.TemporaryDirectory(dir=os.environ.get("TEST_TMPDIR")) as directory:
        root = Path(directory)
        with tarfile.open(package) as archive:
            archive.extractall(root, filter="data")
        packaged = sorted(p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file())
        assert all("external/" not in p and "+" not in p for p in packaged)
        digest = hashlib.sha256()
        for name in packaged:
            file_hash = hashlib.sha256((root / name).read_bytes()).hexdigest()
            digest.update(name.encode("utf-8") + b"\0" + file_hash.encode("ascii") + b"\n")
        if content_id is not None:
            assert digest.hexdigest() == content_id.read_text().strip()
        manifest = json.loads((root / "manifest.json").read_text())
        assert not any("commit" in k or "timestamp" in k or "version" in k
                       for k in manifest if k != "schema_version")
        assert manifest["schema_version"] == 1
        assert manifest["runtime_profile_id"] == "runtime.lua.gizos"
        assert (root / "LICENSE").is_file()
        assert "libs/trie/src/h2_trie.c" in manifest["sources"]
        assert (root / "libs/trie/include/h2_trie.h").is_file()
        assert len(manifest["sources"]) == len(set(manifest["sources"]))
        assert all("/pal/providers/" not in p and "/bleikcp/" not in p for p in manifest["sources"])
        assert "libs/atomic/providers/c11/src/h2_atomic_c11.c" in manifest["sources"]
        compiler = shlex.split(os.environ.get("CC", "cc"))
        flags = manifest["cflags"] + ["-I" + p for p in manifest["include_dirs"]]
        flags += ["-D" + d for d in manifest["defines"]]
        objects = []
        compiled = []
        optimized = {
            "h2_lua_numeric_prepared.c", "h2_lua_geometry_prepared.c",
            "h2_lua_geometry_batches.c", "h2_lua_vmath.c",
            "h2_lua_geometry.c", "h2_lua_display.c",
        }
        seen = set()
        for unit in manifest["compilation_units"]:
            for source in unit["sources"]:
                name = Path(source).name
                if name in optimized:
                    seen.add(name)
                    for flag in ("-O3", "-fno-fast-math"):
                        assert flag in unit["cflags"], (source, flag, unit)
                elif source.startswith("libs/lua/") or source.startswith("libs/raster2d/"):
                    assert "-O3" not in unit["cflags"], (source, unit)
        assert seen == optimized, seen
        for unit in manifest["compilation_units"]:
            for source in unit["sources"]:
                obj = str(root / (str(len(objects)) + ".o"))
                subprocess.run(compiler + flags + unit["cflags"] +
                               ["-D" + d for d in unit["defines"]] +
                               ["-c", source, "-o", obj], cwd=root, check=True)
                objects.append(obj)
                compiled.append(source)
        assert sorted(compiled) == sorted(manifest["sources"])
        executable = str(root / "embedder")
        subprocess.run(compiler + flags + ["-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-pthread", str(harness)] + objects +
                       manifest["per_os"][sys.platform]["link_flags"] +
                       ["-o", executable], cwd=root, check=True)
        subprocess.run([executable], cwd=root, check=True, timeout=30)
        print(f"PASS: {len(compiled)} packaged C sources compiled and linked with {compiler[0]}")


if __name__ == "__main__":
    main()
