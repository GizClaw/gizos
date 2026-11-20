#!/bin/bash
# Read the final card as an ordinary file. The bootloader and partition checks deliberately
# do not use Bazel's intermediate files or the image generator's configuration.
set -euo pipefail
export LC_ALL=C TZ=UTC PATH=/usr/sbin:/usr/bin:/sbin:/bin
image=$(realpath "$1")
work=$(mktemp -d "${TEST_TMPDIR:-${TMPDIR:-/tmp}}/r36s-check.XXXXXXXX")
trap 'rm -rf "$work"' EXIT
cd "$work"
python3 -I - "$image" <<'PY'
import struct
import sys

with open(sys.argv[1], "rb") as card:
    mbr = card.read(512)
    assert mbr[510:] == b"\x55\xaa" and mbr[440:444] == bytes.fromhex("30503336"), "bad MBR"
    for index, expected in enumerate([(0x80, 0x0e, 32768, 131072), (0, 0x83, 163840, 32768)]):
        entry = mbr[446 + 16 * index:462 + 16 * index]
        assert struct.unpack("<B3xB3xII", entry) == expected, f"partition {index + 1}"
    def check_at(offset, signature):
        card.seek(offset)
        assert card.read(len(signature)) == signature, f"signature at 0x{offset:x}"
    check_at(64 * 512, bytes.fromhex("3b8cdcfc"))    # Rockchip IDB header
    check_at(68 * 512, b"RK33")                     # DDR init
    check_at(16384 * 512, b"LOADER  ")               # loaderimage-wrapped U-Boot
    check_at(24576 * 512, b"BL3X")                   # RK trust image
    check_at(163840 * 512, b"hsqs")                  # SquashFS
    assert card.seek(0, 2) == 96 * 1024 * 1024, "truncated or oversized card"
PY
sfdisk --dump "$image"
dd if="$image" of=boot.vfat bs=1M skip=16 count=64 status=none
fsck.fat -n boot.vfat
mcopy -i boot.vfat ::boot.scr ::Image ::r36s.dtb .
dumpimage -l boot.scr
# A legacy script carries a header, a two-word component table, then the U-Boot commands.
python3 -I - <<'PY'
from pathlib import Path
import struct
import zlib

image = Path('boot.scr').read_bytes()
header, payload = image[:64], image[64:]
magic, hcrc, _, size, _, _, dcrc = struct.unpack_from('>7I', header)
assert magic == 0x27051956 and size == len(payload), 'invalid script header'
assert zlib.crc32(header[:4] + bytes(4) + header[8:]) == hcrc, 'script header CRC'
assert zlib.crc32(payload) == dcrc, 'script payload CRC'
length, sentinel = struct.unpack_from('>2I', payload)
assert sentinel == 0 and length == len(payload) - 8, 'script component extent'
Path('boot.cmd').write_bytes(payload[8:])
PY
grep -F 'root=PARTUUID=36335030-02 rootfstype=squashfs rootwait ro' boot.cmd
grep -F 'fatload mmc 1:1 0x09000000 Image && fatload mmc 1:1 0x01f00000 r36s.dtb' boot.cmd
grep -F 'booti 0x09000000 - 0x01f00000' boot.cmd
printf 'ARM\x64' | cmp - <(dd if=Image bs=1 skip=56 count=4 status=none)
test "$(fdtget r36s.dtb / model)" = 'Game Console R36S'
test "$(fdtget r36s.dtb /dsi@ff450000/panel@0 compatible)" = 'gameconsole,r36s-panel sitronix,st7703'
test "$(fdtget r36s.dtb "$(fdtget r36s.dtb /aliases mmc1)" status)" = okay
dd if="$image" of=root.sqfs bs=1M skip=80 status=none
unsquashfs -no-progress -d root root.sqfs >/dev/null
test "$(realpath root/sbin/init)" = "$PWD/root/bin/busybox"
test "$(realpath root/bin/sh)" = "$PWD/root/bin/busybox"
test -x root/etc/rcS
grep -Fx '::sysinit:/etc/rcS' root/etc/inittab
readelf -h root/bin/busybox | grep -F AArch64
readelf -l root/bin/busybox > busybox.segments
if grep -q INTERP busybox.segments; then echo 'BusyBox requires a dynamic loader' >&2; exit 1; fi
sha256sum "$image" Image r36s.dtb root/bin/busybox
