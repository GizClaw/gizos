"""Materialize the declared archive/source input inventory without an SDK."""

import argparse
import hashlib
import json
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--metadata", required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    metadata = json.loads(arguments.metadata)
    paths = set()
    for component in metadata["components"]:
        if component["archive"]:
            paths.add(component["archive"])
        paths.update(component["native_srcs"])
        paths.update(component["headers"])
    metadata["inputs"] = {
        path: hashlib.sha256(Path(path).read_bytes()).hexdigest()
        for path in sorted(paths)
    }
    arguments.output.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
