# GizOS Linux bring-up on Game Console R36S

Build a raw TF1/OS microSD image for the **genuine RK3326 R36S-V12 (2023-08-18), original Panel 0**. Check the board and the original card's DTB. The same PCB marking also ships with other panels, and EE clones have different hardware. The selected 640×480 ST7703 panel uses the built-in `gameconsole,r36s-panel` driver. [Board and panel reference](https://github.com/southoz/dArkOSRE-R36/tree/3f372c85aa53a26b265c3dc0c2661ec0fae5faf0/files/BOOT/dtb/r36s/R36S-V12%202023-08-18%20Panel%200) · [ROCKNIX device notes](https://rocknix.org/devices/unbranded/game-console-r35s-r36s/)

## Build

Use Bazel from `.bazelversion` and a case-sensitive Linux filesystem. On Ubuntu 24.04 ARM64 install the native tools below. On an x86-64 Linux host also install `gcc-aarch64-linux-gnu` and `libc6-dev-arm64-cross` for the target compiler and static C library.

```sh
sudo apt-get install build-essential bc bison flex libssl-dev libelf-dev pkg-config xz-utils bzip2 patch \
    device-tree-compiler genimage dosfstools mtools squashfs-tools faketime fdisk u-boot-tools python3
bazel build --config=rk3326 //projects/linux_bringup/targets/sd_image/gameconsole/r36s:gizos-r36s-panel0
```

The build itself runs unprivileged. Its output is `bazel-bin/projects/linux_bringup/targets/sd_image/gameconsole/r36s/gizos-r36s-panel0.img`. Write the **whole image** to a spare card for TF1 and keep the original OS card. On startup, the console prints `GizOS Linux bring-up | R36S-V12 / original Panel 0`. Press Enter with a USB keyboard or on the UART2 console (1500000 8N1) for a local root shell. The root filesystem is read-only. `/run` and `/tmp` are in RAM. This initial image contains BusyBox and the bring-up script. GizOS graphical Apps and emulators are outside this target.

## Boot chain and layout

Bazel verifies the source archives, downstream patches and binary firmware by SHA-256 before invoking upstream Make. The build compiles ROCKNIX's Hardkernel U-Boot 2017.09 for its legacy RK3326 `a` boot chain, Linux 6.12.17 with the ROCKNIX R36S board, panel and joypad patches, and a statically linked AArch64 BusyBox 1.37.0. The three Rockchip binary inputs are DDR init 333 MHz v2.11, miniloader v1.40 and BL31 v1.34. License notices are in `/usr/share/licenses`. [Immutable source and firmware pins](../../third_party/r36s/repositories.bzl) · [Upstream boot recipe](https://github.com/ROCKNIX/distribution/blob/cefc14339a4ae18c02bb69888f27835e50664b60/projects/ROCKNIX/bootloader/rkhelper)

| Sector (512 bytes) | Contents |
| ---: | --- |
| 0 | MBR, disk signature `36335030` |
| 64 | Rockchip ID block, DDR init and miniloader |
| 16384 | `LOADER`-wrapped U-Boot |
| 24576 | `BL3X` trust image containing BL31 |
| 32768 | 64 MiB FAT16 boot partition with `boot.scr`, `Image` and `r36s.dtb` |
| 163840 | 16 MiB partition containing SquashFS, `PARTUUID=36335030-02` |

The 96 MiB image uses a fixed disk ID, filesystem IDs and timestamps. For byte-for-byte comparison, use the same GNU compiler, static libc and packaging tools in fresh Bazel output bases. Native actions run locally. [Board configuration](../../boards/gameconsole/r36s/rk3326) · [Image target and init policy](targets/sd_image/gameconsole/r36s)

## Verify without a card

```sh
bazel test --config=rk3326 //projects/linux_bringup/targets/sd_image/gameconsole/r36s:image_test
```

The test reads the final image to check its extent, partition table, ROM and firmware headers, boot script, ARM64 Image, R36S DTB and static BusyBox init. To exercise kernel and userspace on an ARM virt machine, install `qemu-system-arm`, extract `Image` from the FAT partition and give the same card to virtio read-only.

```sh
img=$(realpath bazel-bin/projects/linux_bringup/targets/sd_image/gameconsole/r36s/gizos-r36s-panel0.img)
work=$(mktemp -d)
mcopy -i "$img@@16777216" ::Image "$work/Image"
qemu-system-aarch64 -machine virt -cpu cortex-a35 -m 512 -nic none -nographic -no-reboot \
    -kernel "$work/Image" -append 'console=ttyAMA0 root=PARTUUID=36335030-02 rootfstype=squashfs rootwait ro' \
    -drive "if=none,id=os,file=$img,format=raw,readonly=on" -device virtio-blk-device,drive=os
```

At the shell, `uname -r` identifies the kernel, `cat /proc/mounts` shows the SquashFS root and `poweroff -f` exits the guest. The R36S BootROM, DDR training, TF1 interface, panel and physical controls require a matching handheld for qualification.
