# Rockchip Linux image rules

A product can compose these rules through a Bzlmod dependency on GizOS. Each stage accepts labels for its inputs and declares ordinary output files. The consuming repository selects its source and firmware inputs and owns its patches, board wiring, target platform, init policy, flash geometry and final image target. [Public declarations](defs.bzl) · [Consuming GizOS](../../../../README.md#use-gizos-from-bzlmod)

```starlark
load("@gizos//tools/bazel/native/rockchip:defs.bzl", "busybox_squashfs", "rockchip_legacy_uboot", "rockchip_linux_kernel", "rockchip_sd_image")
```

## Optional RK3326 legacy firmware

Import Rockchip's pinned rkbin inputs from the consumer's `MODULE.bazel` when its board uses this legacy boot chain. Bazel verifies each file's SHA-256. The profile provides the RK3326 333 MHz DDR initializer v2.11, miniloader v1.40, BL31 v1.34 and the accompanying Rockchip license. [Version manifest](../versions/rk3326_legacy_rkbin.json) · [Upstream rkbin](https://github.com/rockchip-linux/rkbin/tree/74213af1e952c4683d2e35952507133b61394862)

```starlark
rockchip = use_extension("@gizos//tools/bazel/native/rockchip:repositories.bzl", "rockchip_repositories")
use_repo(rockchip, "gizos_rk3326_legacy_rkbin")
```

Pass `@gizos_rk3326_legacy_rkbin//:ddr.bin`, `:miniloader.bin` and `:bl31.elf` from that repository to the corresponding attributes of `rockchip_legacy_uboot`. Include its `LICENSE` in the product's firmware notices. Consumers with a different validated firmware set can supply their own file labels. The product chooses its U-Boot sources, defconfig and patches, trust configuration, reference digest and card layout.

## Build host and cache

Use a case-sensitive Linux filesystem, Bazel 9.2.0, Python 3.10 or later, host GCC, AArch64 GNU binutils and `aarch64-linux-gnu-gcc`. The stages run as an unprivileged user in private build directories. On Ubuntu 24.04 the host packages for all four stages are `python3`, `build-essential`, `bc`, `bison`, `flex`, `libssl-dev`, `libelf-dev`, `pkg-config`, `xz-utils`, `bzip2`, `patch`, `device-tree-compiler`, `genimage`, `dosfstools`, `mtools`, `faketime` and `squashfs-tools`. On x86-64 also install `gcc-aarch64-linux-gnu` and `libc6-dev-arm64-cross`. Native builds consume trusted archives, patches, Makefiles and genimage configurations.

Register the image profile in the **consumer's** `.bazelrc`. Choose its target platform and App toolchain there as usual. The host tools are provided by the Linux installation, so the four actions take a fresh workspace-status value on every invocation. A tool replaced at the same path is picked up on the next build with the same output base. Bazel can reuse verified source downloads. Reproducing the same output bytes requires the same toolchain contents, inputs, build identity and `source_date_epoch`. The UUID only affects the action key.

```text
build:card --workspace_status_command=tools/card_status.sh
```

In the consumer's executable `tools/card_status.sh`, or its existing workspace status command, emit one UUID per build.

```sh
#!/bin/sh
set -eu
read -r invocation < /proc/sys/kernel/random/uuid
printf 'STABLE_ROCKCHIP_BUILD_ID %s\n' "$invocation"
```

Run `bazel build --config=card //path/to/product:card`. Use `target_compatible_with` on the consumer's board targets to scope its regular build graph. Local image actions select Linux execution.

## Stage contracts

| Rule | Caller-supplied inputs | Outputs |
| --- | --- | --- |
| `rockchip_linux_kernel` | Single-root Linux source archive, ordered `patches`, `config`, Kbuild `dtb_target` and optionally `dts_files` mapping source labels to paths under `arch/arm64/boot/dts/` | `image` and `dtb` |
| `rockchip_legacy_uboot` | Single-root source archive, ordered `patches`, `<board>_defconfig`, DDR init, miniloader, BL31 ELF, `trust_ini`, boot command, script title, `mkimage_soc` and U-Boot `load_address` | `idbloader`, `uboot`, `trust` and `script` |
| `busybox_squashfs` | Single-root BusyBox source archive, `config` over `allnoconfig` with `CONFIG_STATIC=y`, and optional ordered patches, directories, data files, executable files and files from the unpacked archive | `out`, a read-only SquashFS with root-owned files and fixed inode times |
| `rockchip_sd_image` | A genimage `layout`, `layout_image` entry name, and a label-to-staging-path map named `inputs` | `out`, the unmounted raw image |

All stages take `source_date_epoch` in Unix seconds. Kbuild stages accept `build_user` and `build_host` for reproducible version strings. The kernel rule can replace an existing DTS or install a new one. Kernel and BusyBox builds check that each requested `CONFIG_…=…` assignment survived configuration. Each mapped label must produce one file. Output labels from the first three stages can be supplied directly to the disk rule. The rootfs may also come from another package or build system.

The legacy boot rule implements the 64-bit Hardkernel/Rockchip miniloader protocol exercised on the RK3326 legacy layout. It compiles the source tree's `u-boot-dtb.bin`, `mkimage`, `loaderimage` and `trust_merger`, appends the miniloader to an `rksd`-wrapped DDR payload, wraps U-Boot, packs BL31 and creates an AArch64 Linux `IH_TYPE_SCRIPT`. Supply a compatible U-Boot tree and any required tool fixes. The INI refers to `bl31.elf` and writes `trust.img`. `trust_checksum` optionally names a file whose first field is the expected full trust image SHA-256. The consuming board selects its reference, boot addresses and storage offsets. The supported contract covers this legacy protocol. Upstream SPL/FIT, SPI/NAND and 32-bit boot protocols have separate requirements.

Additional rootfs paths are relative to `/`. `files` installs the declared data with mode 0644, and `executables` installs programs or scripts with mode 0755. `source_files` maps files from the extracted archive to data destinations, for example a license notice. The consumer supplies programs suitable for its rootfs and chooses startup, mounts and firmware notices. Paths must be unique and stay inside their staging trees.

## Compose a disk from declared payloads

Here the kernel and firmware rules declare individual named output files. The keys below are Bazel labels. The values are the paths that `card.cfg` reads from the genimage input directory.

```starlark
rockchip_sd_image(
    name = "card",
    layout = "card.cfg",
    layout_image = "system.img",
    inputs = {
        ":kernel_image": "Image",
        ":board.dtb": "board.dtb",
        ":idbloader": "idbloader.img",
        ":bootloader": "uboot.img",
        ":secure_firmware": "trust.img",
        ":boot_script": "boot.scr",
        "//system:root.sqfs": "rootfs.sqfs",
    },
    out = "system-card.img",
    source_date_epoch = 1704067200,
)
```
