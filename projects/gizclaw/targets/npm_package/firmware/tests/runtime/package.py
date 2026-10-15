"""Build a runtime fixture with an actual npm tarball and firmware module."""
import argparse
import io
import json
from pathlib import Path, PurePosixPath
import tarfile

parser = argparse.ArgumentParser()
for name in ("firmware", "package", "host", "assertions", "output"):
    parser.add_argument("--" + name, type=Path, required=True)
args = parser.parse_args()
files = {}
with tarfile.open(args.firmware) as archive:
    for item in archive:
        name = str(PurePosixPath(item.name))
        assert not name.startswith("/") and ".." not in PurePosixPath(name).parts
        assert item.isfile() or item.isdir()
        if item.isfile():
            files[name] = archive.extractfile(item).read()
# Only these test pages auto-start fixtures; the SDK never starts a product.
auto = b"<script>addEventListener('load',()=>document.getElementById('start').click(),{once:true});</script>"
original = files["index.html"]
files["secondary.html"] = original.replace(b"</body>", auto + b"</body>")
files["index.html"] = original.replace(b"</body>", args.host.read_bytes() + auto + b"</body>")
with tarfile.open(next(args.package.glob("*.tgz"))) as archive:
    manifest = json.load(archive.extractfile("package/package.json"))
    assert manifest["exports"]["."]["import"] == "./firmware.js"
    for item in archive:
        if item.isfile():
            assert item.name.startswith("package/")
            name = item.name.removeprefix("package/")
            assert ".." not in PurePosixPath(name).parts
            files["sdk/" + name] = archive.extractfile(item).read()
files["assertions.mjs"] = args.assertions.read_bytes()
with tarfile.open(args.output, "w", format=tarfile.PAX_FORMAT) as archive:
    for name, data in sorted(files.items()):
        item = tarfile.TarInfo(name)
        item.size = len(data)
        item.mode = 0o644
        archive.addfile(item, io.BytesIO(data))
