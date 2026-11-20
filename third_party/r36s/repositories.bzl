"""Verified upstream source inputs for the Game Console R36S image."""

_ROCKNIX = "https://raw.githubusercontent.com/ROCKNIX/distribution/"
_LINUX_BOARD = _ROCKNIX + "fec842b58559ee973a0100abd726370e4a032b77/projects/Rockchip/patches/linux/RK3326/"
_LINUX_COMMON = _ROCKNIX + "fec842b58559ee973a0100abd726370e4a032b77/packages/kernel/linux/patches/mainline/"
_UBOOT_BOARD = _ROCKNIX + "cefc14339a4ae18c02bb69888f27835e50664b60/projects/ROCKNIX/devices/RK3326/packages/u-boot-legacy/"
_UBOOT = "2492a3e467e332e2350d987234ce6123700b3392"
_RKBIN = "https://raw.githubusercontent.com/rockchip-linux/rkbin/74213af1e952c4683d2e35952507133b61394862/"

# Preserve source archives intact. The native actions unpack them on a case-sensitive Linux filesystem.
_INPUTS = {
    "linux.tar.xz": ("https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.12.17.tar.xz", "5c205cd34f80974e4973e321cb008f5f6895a8aa8c2577f06a9448cd77de63b3"),
    "dts.patch": (_LINUX_BOARD + "000-rk3326-dts.patch", "b41b00083fdcfd7862bca6c8b5ead1876e11dd094f4d521d2e17745c7e144a58"),
    "panel.patch": (_LINUX_BOARD + "001-panel-updates.patch", "58b1f9a73ebfa36e249c4dc527a8f57c5f4cfcd14dcf594cd70875c61ecc6825"),
    "gpio.patch": (_LINUX_COMMON + "0001-gpiolib-of-revert-api-changes-needed-for-joypad-driv.patch", "38f9246adfda80c9c50d061b4f935317ec4f62cfbcc3f16ceb4ef844113cb8aa"),
    "polldev.patch": (_LINUX_COMMON + "0002-input-add-input-polldev-driver.patch", "aa49ec749c71cd97f02a9236ef0d42780229fcb6ee59d6b07ff8acd3abb72303"),
    "joypad.patch": (_LINUX_BOARD + "004-input-drivers.patch", "27d2c92560ac22652763f587214a920641ff9bc7e185acc21b4f1058b26c3dc5"),
    "uboot.tar.gz": ("https://github.com/ROCKNIX/hardkernel-uboot/archive/" + _UBOOT + ".tar.gz", "d8b358f20394ff1c901af989d0a5b7740f46708bd93ff714459d7bc3794a72f8"),
    "uboot.patch": (_UBOOT_BOARD + "patches/01-fix-building.patch", "b051d4ed375c6906317eeddf538e0e3d1d87088a62e58a0432c7960bd990201a"),
    "rocknix_rk3326_defconfig": (_UBOOT_BOARD + "sources/configs/rocknix_rk3326_defconfig", "f2b66d895ee7c67639f8eb526466079d72678f84123709aea8b44cb6f203d738"),
    "ddr.bin": (_RKBIN + "bin/rk33/rk3326_ddr_333MHz_v2.11.bin", "2df577824953bea3584282e7f5118c96620e1348dbc378d50604fe07c70eb3e4"),
    "miniloader.bin": (_RKBIN + "bin/rk33/rk3326_miniloader_v1.40.bin", "473aacd5378f356757a71efeb54f4339925db154fbce288df7e3c1177f45bc28"),
    "bl31.elf": (_RKBIN + "bin/rk33/rk3326_bl31_v1.34.elf", "387aaee716a6753a9e6dd18c72b27783d125a294f6aa817531a5a6f340f16080"),
    "rkbin.LICENSE": (_RKBIN + "LICENSE", "d2765124a2c24d71aba3cbc6cdd6975a5a297821264eb6c26378a4c16cd16e92"),
    "busybox.tar.bz2": ("https://busybox.net/downloads/busybox-1.37.0.tar.bz2", "3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4"),
}

def _sources(ctx):
    downloads = [ctx.download(url, name, sha256 = digest, block = False) for name, (url, digest) in _INPUTS.items()]
    for download in downloads:
        download.wait()
    ctx.file("BUILD.bazel", "exports_files(['bl31.elf'])\nfilegroup(name = 'inputs', srcs = %s, visibility = ['//visibility:public'])\n" % repr(sorted(_INPUTS.keys())))

r36s_sources = repository_rule(implementation = _sources)
