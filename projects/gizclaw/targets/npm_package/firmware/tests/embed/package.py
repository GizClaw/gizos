"""Build a test-only host with an actual npm tarball and separate firmware."""
import argparse
import io
import json
from pathlib import Path, PurePosixPath
import tarfile

parser = argparse.ArgumentParser()
for name in ("firmware", "package", "host", "assertions", "protocol", "output"):
    parser.add_argument("--" + name, type=Path, required=True)
args = parser.parse_args()
files = {}
with tarfile.open(args.firmware) as archive:
    for item in archive:
        name = str(PurePosixPath(item.name))
        assert not name.startswith("/") and ".." not in PurePosixPath(name).parts
        assert item.isfile() or item.isdir()
        if item.isfile():
            files["firmware/" + name] = archive.extractfile(item).read()
# Only this test host auto-starts its fixtures; the SDK never starts a product.
auto = b"<script>addEventListener('load',()=>document.getElementById('start').click(),{once:true});</script>"
files["firmware/index.html"] = files["firmware/index.html"].replace(b"</body>", auto + b"</body>")
with tarfile.open(next(args.package.glob("*.tgz"))) as archive:
    manifest = json.load(archive.extractfile("package/package.json"))
    assert manifest["exports"]["."]["import"] == "./firmware.js"
    for item in archive:
        if item.isfile():
            assert item.name.startswith("package/")
            name = item.name.removeprefix("package/")
            assert ".." not in PurePosixPath(name).parts
            files["sdk/" + name] = archive.extractfile(item).read()
files["index.html"] = args.host.read_bytes()
files["assertions.mjs"] = args.assertions.read_bytes()
files["protocol.json"] = args.protocol.read_bytes()
files["_headers"] = files["firmware/_headers"] + b"  Cross-Origin-Resource-Policy: cross-origin\n"
with tarfile.open(args.output, "w", format=tarfile.PAX_FORMAT) as archive:
    for name, data in sorted(files.items()):
        item = tarfile.TarInfo(name)
        item.size = len(data)
        item.mode = 0o644
        archive.addfile(item, io.BytesIO(data))
