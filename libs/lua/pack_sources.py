"""Create a deterministic source archive from the Bazel aspect's inventory."""

import gzip
import io
import json
import sys
import tarfile
from pathlib import Path


def main():
    manifest = json.loads(Path(sys.argv[1]).read_text())
    files = manifest.pop("files")
    with open(sys.argv[2], "wb") as output:
        with gzip.GzipFile(filename="", mode="wb", fileobj=output, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w") as archive:
                contents = {name: Path(path).read_bytes() for name, path in files.items()}
                contents["manifest.json"] = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode()
                for name, data in sorted(contents.items()):
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    info.mode = 0o644
                    archive.addfile(info, io.BytesIO(data))


if __name__ == "__main__":
    main()
