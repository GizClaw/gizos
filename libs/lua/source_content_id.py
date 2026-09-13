"""Hash the canonical uncompressed package file listing."""

import hashlib
import json
from pathlib import Path
import sys


def main():
    files = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    digest = hashlib.sha256()
    for name in sorted(files):
        file_hash = hashlib.sha256(Path(files[name]).read_bytes()).hexdigest()
        digest.update(name.encode("utf-8") + b"\0" + file_hash.encode("ascii") + b"\n")
    Path(sys.argv[2]).write_text(digest.hexdigest() + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
