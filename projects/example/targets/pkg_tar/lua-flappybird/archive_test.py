import pathlib
import sys
import tarfile


def main() -> int:
    archive = pathlib.Path(sys.argv[1])
    with tarfile.open(archive) as bundle:
        names = sorted(member.name.lstrip("./") for member in bundle.getmembers() if member.isfile())
        html_member = next(
            member for member in bundle.getmembers()
            if member.isfile() and member.name.lstrip("./") == "index.html"
        )
        html = bundle.extractfile(html_member).read().decode("utf-8")
    expected = ["index.html", "index.js", "index.wasm"]
    if names != expected:
        raise AssertionError(f"unexpected archive inventory: {names}")
    if "<button" in html.lower():
        raise AssertionError("Flappy Bird shell must not render Back/Stop buttons")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
