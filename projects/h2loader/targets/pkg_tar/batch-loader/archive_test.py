import sys
import tarfile


def main() -> None:
    with tarfile.open(sys.argv[1]) as archive:
        names = {member.name.lstrip("./") for member in archive.getmembers()}
        required = {
            "index.html",
            "_headers",
            "sdk/h2loader.js",
            "sdk/h2loader_runtime.js",
            "sdk/h2loader.wasm",
        }
        assert required <= names, (required, names)
        html = archive.extractfile(next(m for m in archive.getmembers()
                                        if m.name.lstrip("./") == "index.html")).read()
        assert b'<div id="root"></div>' in html
        assert b'<canvas id="canvas"' not in html
        assert not any(name.endswith("index.data") for name in names)
        assert not any("lvgl" in name.lower() for name in names)
        assert not any(name.endswith(("package.json", "pnpm-lock.yaml"))
                       for name in names)
        headers = archive.extractfile(next(m for m in archive.getmembers()
                                           if m.name.lstrip("./") == "_headers")).read()
        assert b"Permissions-Policy: serial=(self)" in headers
        assert b"Content-Security-Policy:" in headers
        assert b"X-Content-Type-Options: nosniff" in headers
        assert b"'unsafe-inline'" not in headers
        wasm = archive.extractfile(next(m for m in archive.getmembers()
                                        if m.name.lstrip("./") == "sdk/h2loader.wasm")).read(4)
        assert wasm == b"\x00asm"


if __name__ == "__main__":
    main()
