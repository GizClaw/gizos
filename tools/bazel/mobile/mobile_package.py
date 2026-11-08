"""Deterministic SDK envelopes around Bazel-produced native binaries."""
import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import plistlib
import posixpath
import re
import struct
import zipfile


def archive_bytes(files):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, content in sorted(files.items()):
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, content)
    return buffer.getvalue()


def public_headers(paths):
    files = {}
    for path in paths:
        name = path.split("/include/", 1)[1]
        if name in files:
            raise ValueError(f"duplicate public header: {name}")
        files[name] = Path(path).read_bytes()
    # Preserve the public hierarchy and make quoted includes relocatable even
    # when Swift/Clang imports the framework without a global header search path.
    for name, content in files.items():
        def relative(match):
            target = match.group(1)
            return '#include "' + (posixpath.relpath(target, posixpath.dirname(name) or ".") if target in files else target) + '"'
        files[name] = re.sub(r'#include "([^"]+)"', relative, content.decode()).encode()
    return files


def swift_package(args, headers):
    with zipfile.ZipFile(args.binary) as original:
        files = {n: original.read(n) for n in original.namelist() if not n.endswith("/")}
    root = args.name + ".xcframework/"
    info = plistlib.loads(files[root + "Info.plist"])
    slices = info["AvailableLibraries"]
    required = {("ios", "", ("arm64",)), ("ios", "simulator", ("arm64",))}
    actual = {(s["SupportedPlatform"], s.get("SupportedPlatformVariant", ""), tuple(s["SupportedArchitectures"])) for s in slices}
    if actual != required:
        raise ValueError(f"unexpected XCFramework slices: {actual}")
    for entry in slices:
        framework = root + entry["LibraryIdentifier"] + "/" + entry["LibraryPath"] + "/"
        files = {n: b for n, b in files.items() if not n.startswith((framework + "Headers/", framework + "Modules/"))}
        files.update({framework + "Headers/" + n: b for n, b in headers.items()})
        files[framework + "Modules/module.modulemap"] = (f'framework module {args.name} {{\n'
            '  umbrella header "h2_ios_platform.h"\n  export *\n  module * { export * }\n'
            '  link framework "UIKit"\n  link framework "Foundation"\n'
            '  link framework "CoreGraphics"\n  link framework "CoreBluetooth"\n  link framework "Security"\n  link "c++"\n}\n').encode()
    bundle = Path(args.bundle)
    for name, content in files.items():
        target = bundle / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
    (bundle / "Package.swift").write_text('// swift-tools-version: 5.9\nimport PackageDescription\n\n'
        f'let package = Package(name: "{args.name}", platforms: [.iOS(.v16)],\n'
        f'    products: [.library(name: "{args.name}", targets: ["{args.name}"])],\n'
        f'    targets: [.binaryTarget(name: "{args.name}", path: "{args.name}.xcframework")])\n')
    return archive_bytes(files), {"slices": slices}


def android_ident(binary):
    """Read API/NDK from the linked ELF, never guess from the build machine."""
    if binary[:6] != b"\x7fELF\x02\x01" or int.from_bytes(binary[18:20], "little") != 183:
        raise ValueError("Android AAR requires a little-endian ELF64 AArch64 library")
    section_offset = struct.unpack_from("<Q", binary, 40)[0]
    entry_size, count = struct.unpack_from("<HH", binary, 58)
    for index in range(count):
        section = section_offset + index * entry_size
        if struct.unpack_from("<I", binary, section + 4)[0] != 7:  # SHT_NOTE
            continue
        offset, size = struct.unpack_from("<QQ", binary, section + 24)
        end = offset + size
        while offset + 12 <= end:
            name_size, desc_size, kind = struct.unpack_from("<III", binary, offset)
            name = binary[offset + 12:offset + 12 + name_size].rstrip(b"\0")
            desc = offset + 12 + ((name_size + 3) & ~3)
            if name == b"Android" and kind == 1 and desc_size >= 68:
                api = struct.unpack_from("<I", binary, desc)[0]
                ndk_version = binary[desc + 4:desc + 68].split(b"\0")[0].decode()
                ndk_major = int(re.fullmatch(r"r(\d+)[a-z]*", ndk_version)[1])
                return api, ndk_major, ndk_version
            offset = desc + ((desc_size + 3) & ~3)
    raise ValueError("Android ELF is missing its API/NDK identity note")


def android_package(args, headers):
    binary = Path(args.binary).read_bytes()
    api, ndk_major, ndk_version = android_ident(binary)
    module = "prefab/modules/h2_pal_core/"
    files = {
        "AndroidManifest.xml": f'<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="com.haivivi.gizos.pal.core"><uses-sdk android:minSdkVersion="{api}"/></manifest>'.encode(),
        "classes.jar": archive_bytes({}),
        "jni/arm64-v8a/libh2_pal_core.so": binary,
        "prefab/prefab.json": json.dumps({"schema_version": 2, "name": "h2_pal_core", "version": args.version, "dependencies": []}).encode(),
        module + "module.json": json.dumps({"export_libraries": ["-llog", "-landroid", "-ljnigraphics", "-laaudio", "-lmediandk"], "library_name": "libh2_pal_core"}).encode(),
        module + "libs/android.arm64-v8a/abi.json": json.dumps({"abi": "arm64-v8a", "api": api, "ndk": ndk_major, "stl": "c++_static", "static": False}).encode(),
        module + "libs/android.arm64-v8a/libh2_pal_core.so": binary,
    }
    files.update({module + "include/" + n: b for n, b in headers.items()})
    bundle = Path(args.bundle)
    bundle.mkdir(parents=True, exist_ok=True)
    (bundle / "h2-pal-core.pom").write_text('<?xml version="1.0" encoding="UTF-8"?>\n'
        '<project xmlns="http://maven.apache.org/POM/4.0.0"><modelVersion>4.0.0</modelVersion>'
        '<groupId>com.haivivi.gizos</groupId><artifactId>h2-pal-core</artifactId>'
        f'<version>{args.version}</version><packaging>aar</packaging></project>\n')
    return archive_bytes(files), {"abis": ["arm64-v8a"], "min_api": api, "ndk": ndk_version}


def package(args):
    headers = public_headers(args.header)
    content, metadata = (swift_package if args.kind == "swift" else android_package)(args, headers)
    Path(args.artifact).write_bytes(content)
    checksum = hashlib.sha256(content).hexdigest()
    bundle = Path(args.bundle)
    (bundle / "checksum.txt").write_text(checksum + "\n")
    metadata.update(name=args.name, version=args.version, sha256=checksum,
                    artifact=Path(args.artifact).name, header_count=len(headers))
    (bundle / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")


def extract(args):
    with zipfile.ZipFile(args.archive) as archive:
        Path(args.binary).parent.mkdir(parents=True, exist_ok=True)
        Path(args.binary).write_bytes(archive.read(args.member))
        found = False
        for name in archive.namelist():
            if not name.startswith(args.header_prefix) or name.endswith("/"):
                continue
            relative = PurePosixPath(name[len(args.header_prefix):])
            if relative.is_absolute() or ".." in relative.parts:
                raise ValueError("unsafe archive path")
            target = Path(args.headers) / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(archive.read(name))
            found = True
        if not found:
            raise ValueError("archive has no public headers")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    pack = commands.add_parser("package")
    for name in ("kind", "binary", "name", "version", "artifact", "bundle"):
        pack.add_argument("--" + name, required=True)
    pack.add_argument("--header", action="append", default=[])
    unpack = commands.add_parser("extract")
    for name in ("archive", "member", "header-prefix", "binary", "headers"):
        unpack.add_argument("--" + name, required=True)
    args = parser.parse_args()
    (package if args.command == "package" else extract)(args)

if __name__ == "__main__":
    main()
