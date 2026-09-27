#!/usr/bin/env python3
"""Compile and link a Swift consumer of the generated local binary package."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("package", type=Path)
parser.add_argument("--log", type=Path, default=Path("/tmp/pal-core-swift-consumer.log"))
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="pal-core-swift-") as temporary:
    root = Path(temporary)
    shutil.copytree(args.package, root / "SDK")
    (root / "Sources/Smoke").mkdir(parents=True)
    (root / "Package.swift").write_text('''// swift-tools-version: 5.9
import PackageDescription
let package = Package(name: "PALCorePackageSmoke", platforms: [.iOS(.v16)],
    products: [.library(name: "Smoke", type: .dynamic, targets: ["Smoke"])],
    dependencies: [.package(path: "SDK")],
    targets: [.target(name: "Smoke", dependencies: [.product(name: "H2PALCore", package: "SDK")])])
''')
    (root / "Sources/Smoke/Smoke.swift").write_text('''import H2PALCore
public func palCoreAvailable() -> Bool {
    return h2_ios_platform_task_api() != nil && h2_ios_platform_sync_api() != nil
        && h2_ios_platform_timer_api() != nil && h2_ios_platform_core_shutdown() == H2_PAL_OK
}
''')
    with args.log.open("w") as log:
        subprocess.run(["xcodebuild", "-scheme", "PALCorePackageSmoke", "-destination",
                        "generic/platform=iOS Simulator", "-derivedDataPath", str(root / "DerivedData"),
                        "build", "CODE_SIGNING_ALLOWED=NO", "ARCHS=arm64", "ONLY_ACTIVE_ARCH=YES"],
                       cwd=root, stdout=log, stderr=subprocess.STDOUT, timeout=300, check=True)
print(f"Swift Package consumer compiled and linked: {args.log}")
