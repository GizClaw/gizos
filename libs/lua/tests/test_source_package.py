"""Extract and compile using only manifest.json, a host C compiler and a harness.

Standalone: python3 test_source_package.py PACKAGE.tar.gz [test_embedder.c]
No Bazel invocation or repository source discovery occurs in this test.
"""

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
    with tempfile.TemporaryDirectory(dir=os.environ.get("TEST_TMPDIR")) as directory:
        root = Path(directory)
        with tarfile.open(package) as archive:
            archive.extractall(root, filter="data")
        manifest = json.loads((root / "manifest.json").read_text())
        assert manifest["schema_version"] == 1
        assert manifest["runtime_profile_id"] == "runtime.lua.gizos"
        assert (root / "LICENSE").is_file()
        assert len(manifest["sources"]) == len(set(manifest["sources"]))
        assert all("/providers/" not in p and "/bleikcp/" not in p for p in manifest["sources"])
        compiler = shlex.split(os.environ.get("CC", "cc"))
        flags = manifest["cflags"] + ["-I" + p for p in manifest["include_dirs"]]
        flags += ["-D" + d for d in manifest["defines"]]
        objects = []
        compiled = []
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
