import pathlib
import sys
import tarfile


def main() -> int:
    with tarfile.open(pathlib.Path(sys.argv[1])) as bundle:
        names = sorted(member.name.lstrip("./") for member in bundle.getmembers() if member.isfile())
    if names != ["index.html", "index.js", "index.wasm"]:
        raise AssertionError(f"unexpected archive inventory: {names}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
