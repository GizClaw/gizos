"""Native ARM64 Linux, legacy U-Boot, BusyBox and genimage stages."""

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


class Stage:
    def __init__(self, execroot, work, env):
        self.execroot = execroot
        self.work = work
        self.env = env
        self.jobs = min(len(os.sched_getaffinity(0)), 8)

    def input(self, name):
        return self.execroot / name

    def run(self, *command, **options):
        subprocess.run(command, cwd=self.work, env=self.env, check=True, **options)

    def unpack(self, params):
        self.run("tar", "--extract", "--strip-components=1", "--file", self.input(params["archive"]))
        for patch in params["patches"]:
            with self.input(patch).open("rb") as source:
                self.run("patch", "--batch", "--fuzz=0", "-p1", stdin=source)

    def configure_make(self, arch, params):
        self.env.update(ARCH=arch, CROSS_COMPILE="aarch64-linux-gnu-", LOCALVERSION="")
        self.env.update(KBUILD_BUILD_USER=params.get("build_user", "builder"),
                        KBUILD_BUILD_HOST=params.get("build_host", "builder"), KBUILD_BUILD_VERSION="1")
        self.env["KBUILD_BUILD_TIMESTAMP"] = subprocess.check_output(
            ["date", "-u", "-d", "@" + self.env["SOURCE_DATE_EPOCH"], "+%a %b %d %T UTC %Y"],
            text=True, env=self.env).strip()


def relative_path(name):
    """Consumer destinations are literal, nonempty paths underneath a staging directory."""
    if any(part in ("", ".", "..") for part in name.split("/")) or "\x00" in name:
        raise ValueError(f"Invalid relative path: {name!r}")
    return Path(name)


def child(root, name):
    path = root / relative_path(name)
    # The tree may contain symlinks from a source archive or BusyBox's installer.
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError(f"Destination escapes staging tree: {name}")
    return path


def install(root, entries, mode, replace=False):
    destinations = set()
    for source, destination in entries:
        target = child(root, destination)
        if destination in destinations or target.is_symlink() or (target.exists() and not replace):
            raise ValueError(f"Duplicate or existing destination: {destination}")
        destinations.add(destination)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        target.chmod(mode)


def config_settings(config):
    return [line for line in config.read_text().splitlines() if line.startswith("CONFIG_") and "=" in line]


def check_config(requested, generated):
    missing = set(requested).difference(generated.read_text().splitlines())
    if missing:
        raise ValueError("Unresolved Kconfig settings: " + ", ".join(sorted(missing)))


def linux_kernel(stage, params):
    stage.unpack(params)
    stage.configure_make("arm64", params)
    config = stage.input(params["config"])
    shutil.copyfile(config, stage.work / ".config")
    stage.run("make", "-s", "olddefconfig")
    check_config(config_settings(config), stage.work / ".config")
    dt_dir = stage.work / "arch/arm64/boot/dts"
    install(dt_dir, [(stage.input(src), dst) for src, dst in params["dts_files"]], 0o644, replace=True)
    target = params["dtb_target"]
    dtb = child(dt_dir, target)
    if dtb.suffix != ".dtb" or target.startswith("-"):
        raise ValueError(f"Expected a Kbuild DTB target: {target!r}")
    stage.run("make", "-s", f"-j{stage.jobs}", "Image", target)
    return [stage.work / "arch/arm64/boot/Image", dtb]


def legacy_uboot(stage, params):
    soc, address, title = params["mkimage_soc"], params["load_address"], params["script_name"]
    if not re.fullmatch(r"[A-Za-z0-9_-]+", soc) or not re.fullmatch(r"0[xX][0-9a-fA-F]{1,8}", address):
        raise ValueError("Invalid mkimage SoC token or 32-bit U-Boot load address")
    if not title or not title.isascii() or len(title) > 32:
        raise ValueError("Legacy boot script names must have 1 to 32 ASCII characters")
    stage.unpack(params)
    stage.configure_make("arm", params)
    # Keep the effective defconfig name from the declared input. Some vendor
    # Makefiles and stamped configs use it to select the board.
    config = stage.input(params["defconfig"])
    if not re.fullmatch(r"[a-zA-Z0-9_+.-]+_defconfig", config.name) or config.name.startswith("-"):
        raise ValueError("The U-Boot config file must be named <board>_defconfig")
    shutil.copyfile(config, stage.work / "configs" / config.name)
    stage.run("make", "-s", config.name)
    stage.run("make", "-s", f"-j{stage.jobs}", "HOSTCFLAGS=-O2 -fcommon",
              "CONFIG_MKIMAGE_DTC_PATH=scripts/dtc/dtc", "u-boot-dtb.bin", "tools")
    stage.run("tools/mkimage", "-n", soc, "-T", "rksd", "-d", stage.input(params["ddr"]), "-C", "bzip2", "idbloader.img")
    idb = stage.work / "idbloader.img"
    with idb.open("ab") as output, stage.input(params["miniloader"]).open("rb") as loader:
        shutil.copyfileobj(loader, output)
    stage.run("tools/loaderimage", "--pack", "--uboot", "u-boot-dtb.bin", "uboot.img", address)
    # The legacy packer stores paths in short fixed-size fields.
    shutil.copyfile(stage.input(params["bl31"]), stage.work / "bl31.elf")
    shutil.copyfile(stage.input(params["trust_ini"]), stage.work / "trust.ini")
    stage.run("tools/trust_merger", "trust.ini")
    trust = stage.work / "trust.img"
    if params["trust_checksum"]:
        words = stage.input(params["trust_checksum"]).read_text().split()
        if not words or hashlib.sha256(trust.read_bytes()).hexdigest() != words[0]:
            raise ValueError("Complete trust.img SHA-256 differs from the declared checksum")
    stage.run("tools/mkimage", "-A", "arm64", "-O", "linux", "-T", "script", "-C", "none",
              "-n", title, "-d", stage.input(params["boot_command"]), "boot.scr")
    return [idb, stage.work / "uboot.img", trust, stage.work / "boot.scr"]


def busybox_rootfs(stage, params):
    stage.unpack(params)
    stage.configure_make("arm64", {})
    stage.env["KCONFIG_NOTIMESTAMP"] = "1"
    stage.run("make", "-s", "allnoconfig", stdout=subprocess.DEVNULL)
    config = stage.work / ".config"
    # BusyBox allnoconfig resets KCONFIG_ALLCONFIG. Replace its defaults before
    # resolving the selected applets. Preserve config order for the same inputs.
    requested = config_settings(stage.input(params["config"]))
    keys = {setting.split("=", 1)[0] for setting in requested}
    baseline = [line for line in config.read_text().splitlines()
                if line.removeprefix("# ").split("=", 1)[0].removesuffix(" is not set") not in keys]
    config.write_text("\n".join(baseline + requested) + "\n")
    stage.run("scripts/kconfig/conf", "-D", ".config", "Config.in", stdout=subprocess.DEVNULL)
    check_config(requested + ["CONFIG_STATIC=y"], config)
    stage.run("make", "-s", f"-j{stage.jobs}", "busybox")
    root = stage.work / "root"
    stage.run("make", "-s", f"CONFIG_PREFIX={root}", "install")
    for directory in params["directories"]:
        child(root, directory).mkdir(parents=True, exist_ok=True)
    install(root, [(child(stage.work, src), dst) for src, dst in params["source_files"].items()], 0o644)
    for name, mode in (("files", 0o644), ("executables", 0o755)):
        install(root, [(stage.input(src), dst) for src, dst in params[name]], mode)
    epoch = stage.env.pop("SOURCE_DATE_EPOCH")
    stage.run("mksquashfs", root, "rootfs.sqfs", "-comp", "gzip", "-all-root", "-all-time", epoch,
              "-mkfs-time", epoch, "-noappend", "-no-xattrs", "-no-progress", "-processors", "1")
    return [stage.work / "rootfs.sqfs"]


def sd_image(stage, params):
    inputs = stage.work / "input"
    empty = stage.work / "empty-root"
    output = stage.work / "output"
    for directory in (inputs, empty, output):
        directory.mkdir()
    install(inputs, [(stage.input(src), dst) for src, dst in params["inputs"]], 0o644)
    epoch = int(stage.env["SOURCE_DATE_EPOCH"])
    for path in inputs.rglob("*"):
        os.utime(path, (epoch, epoch))
    stamp = subprocess.check_output(["date", "-u", "-d", f"@{epoch}", "+%F %T"], text=True, env=stage.env).strip()
    stage.run("faketime", "-f", stamp, "genimage", "--config", stage.input(params["layout"]),
              "--rootpath", empty, "--tmppath", "gen-tmp", "--inputpath", inputs, "--outputpath", output)
    return [child(output, params["layout_image"])]


def check_invocation(status):
    pattern = r"^STABLE_ROCKCHIP_BUILD_ID [0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$"
    if not re.search(pattern, status, flags=re.MULTILINE):
        raise ValueError("Consumer workspace_status_command must emit a fresh STABLE_ROCKCHIP_BUILD_ID UUID per build")


def main():
    stage_name, status_path, payload = sys.argv[1:]
    check_invocation(Path(status_path).read_text())
    params = json.loads(payload)
    outputs = [Path(name).absolute() for name in params["outputs"]]
    for output in outputs:
        output.parent.mkdir(parents=True, exist_ok=True)
    functions = {"Kernel": linux_kernel, "LegacyUBoot": legacy_uboot, "BusyBoxSquashfs": busybox_rootfs, "SDImage": sd_image}
    with tempfile.TemporaryDirectory(prefix=".rockchip-", dir=outputs[0].parent) as scratch:
        os.umask(0o022)
        stage = Stage(Path.cwd(), Path(scratch), os.environ.copy())
        results = functions[stage_name](stage, params)
        for result in results:
            if not result.is_file():
                raise ValueError(f"Missing native output: {result.name}")
        for result, output in zip(results, outputs, strict=True):
            result.replace(output)


if __name__ == "__main__":
    main()
