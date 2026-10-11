"""Generate public-format Lua app archives and adversarial stream fixtures."""
import argparse
import base64
import hashlib
import io
import json
import pathlib
import tarfile
import zlib


def package(version="1.0.0", *, source=b'assert(args.mode == "single")\n',
            name="demo.lua", hash_bad=False, extra=False, duplicate=False,
            terminator=1024, trailing=b"", data=False, unicode_name=False,
            app_id="demo"):
    files = {name: source}
    if data:
        files["demo/资源.txt" if unicode_name else "demo/nested/readme.txt"] = b"immutable resource"
    manifest = {"format": 1, "type": "lua-app", "app_id": app_id,
                "version": version, "entry": app_id + ".lua",
                "data_dir": "demo" if data else None,
                "files": [{"path": key, "size": len(value),
                           "sha256": "0" * 64 if hash_bad else hashlib.sha256(value).hexdigest()}
                          for key, value in files.items()]}
    entries = sorted({**files, "manifest.json": json.dumps(manifest).encode()}.items())
    if extra:
        entries += [("unlisted.txt", b"extra")]
    if duplicate:
        entries += [(name, source)]
    stream = io.BytesIO()
    for key, value in entries:
        info = tarfile.TarInfo(key)
        info.size = len(value)
        stream.write(info.tobuf(format=tarfile.USTAR_FORMAT))
        stream.write(value)
        stream.write(bytes(-len(value) % 512))
    stream.write(bytes(terminator))
    stream.write(trailing)
    return zlib.compress(stream.getvalue()), manifest


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    fixtures = {}
    for name, options in {
        "valid": {}, "update": {"version": "2.0.0"},
        "alias_id": {"app_id": "demo.", "name": "demo..lua"},
        "data": {"data": True}, "unicode": {"data": True, "unicode_name": True}, "hash": {"hash_bad": True},
        "extra": {"extra": True}, "duplicate": {"duplicate": True},
        "truncated_tar": {"terminator": 512}, "unaligned_tar": {"trailing": b"\0"},
        "trailing_tar": {"trailing": b"x" * 512}, "traversal": {"name": "../demo.lua"},
        "nul": {"source": b"a\0b"}, "bytecode": {"source": b"\x1bLua"},
        "large": {"source": b"--" + b"x" * (300 * 1024)},
        "stream": {"source": b"--" + base64.b85encode(hashlib.shake_256(b"stream-fixture").digest(180000)) + b"\nreturn 1\n"},
    }.items():
        archive, _ = package(**options)
        fixtures[name] = archive
    fixtures["truncated_zlib"] = fixtures["valid"][:-1]
    fixtures["trailing_zlib"] = fixtures["valid"] + b"garbage"
    bad_magic = bytearray(zlib.decompress(fixtures["valid"]))
    bad_magic[262] = ord("X")
    bad_magic[148:156] = b" " * 8
    checksum = sum(bad_magic[:512])
    bad_magic[148:156] = f"{checksum:06o}\0 ".encode()
    fixtures["magic"] = zlib.compress(bad_magic)
    _, manifest = package(version="0.1.0")
    index = json.dumps({"format": 1, "apps": [{"root": "/preinstalled", "manifest": manifest}]}).encode()
    with args.output.open("w") as out:
        out.write("/* Generated from the public format by Python tarfile/zlib/hashlib. */\n")
        for key, value in {**fixtures, "preinstalled_index": index}.items():
            out.write(f"static const unsigned char fixture_{key}[] = {{" + ",".join(map(str, value)) + "};\n")
        for scheme in ("application/zlib", "application/octet-stream"):
            label = "zlib" if scheme.endswith("zlib") else "octet"
            uri = "data:" + scheme + ";base64," + base64.b64encode(fixtures["valid"]).decode()
            out.write(f'static const char fixture_data_{label}[] = "{uri}";\n')
        for name in ("valid", "stream"):
            digest = hashlib.sha256(fixtures[name]).hexdigest()
            out.write(f'static const char fixture_{name}_sha256[] = "{digest}";\n')


if __name__ == "__main__":
    main()
