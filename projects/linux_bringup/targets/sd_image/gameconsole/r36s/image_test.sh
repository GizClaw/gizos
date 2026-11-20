#!/bin/bash
# Exercise the final card as a regular file. No mounts or block devices are needed.
set -euo pipefail
export LC_ALL=C TZ=UTC PATH=/usr/sbin:/usr/bin:/sbin:/bin
image=$(realpath "$1")
checker=$(realpath "$2")
bl31=$(realpath "$3")
trust_pin=$(realpath "$4")
boot_cmd=$(realpath "$5")
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
    assert mbr[478:510] == bytes(32), "unexpected partitions"
    assert card.seek(0, 2) == 96 * 1024 * 1024, "truncated or oversized card"
PY
sfdisk --dump "$image"
dd if="$image" of=boot.vfat bs=1M skip=16 count=64 status=none
fsck.fat -n boot.vfat
mcopy -i boot.vfat ::boot.scr ::Image ::r36s.dtb .
dumpimage -l boot.scr
python3 -I "$checker" "$image" "$bl31" "$trust_pin" boot.scr "$boot_cmd"
printf 'ARM\x64' | cmp - <(dd if=Image bs=1 skip=56 count=4 status=none)

prop() { fdtget r36s.dtb "$1" "$2"; }
link_to() {
    test "$(fdtget -t x r36s.dtb "$1" "$2")" = "$(fdtget -t x r36s.dtb "$3" phandle)"
}
test "$(prop / model)" = 'Game Console R36S'
test "$(prop /dsi@ff450000/panel@0 compatible)" = 'gameconsole,r36s-panel sitronix,st7703'
test "$(prop /aliases mmc1)" = /mmc@ff370000
test "$(prop /mmc@ff370000 status)" = okay
test "$(prop /aliases serial2)" = /serial@ff160000
test "$(prop /serial@ff160000 status)" = okay
otg=/usb@ff300000
phy=/syscon@ff2c0000/usb2phy@100/otg-port
pmic=/i2c@ff180000/pmic@20
test "$(prop "$otg" status)" = okay
test "$(prop "$otg" dr_mode)" = host
test "$(prop "$phy" status)" = okay
link_to "$otg" phys "$phy"
link_to "$otg" vbus-supply "$pmic/regulators/OTG_SWITCH"
link_to "$pmic" vcc9-supply "$pmic/regulators/BOOST"

dd if="$image" of=root.sqfs bs=1M skip=80 status=none
unsquashfs -no-progress -d root root.sqfs >/dev/null
test "$(realpath root/sbin/init)" = "$PWD/root/bin/busybox"
test "$(realpath root/bin/sh)" = "$PWD/root/bin/busybox"
test -x root/etc/rcS
grep -Fx '::sysinit:/etc/rcS' root/etc/inittab
grep -Fx '::askfirst:-/bin/sh' root/etc/inittab
grep -Fx 'tty1::askfirst:-/bin/sh' root/etc/inittab
readelf -h root/bin/busybox | grep -F AArch64
readelf -l root/bin/busybox > busybox.segments
if grep -q INTERP busybox.segments; then echo 'BusyBox requires a dynamic loader' >&2; exit 1; fi
sha256sum "$image" Image r36s.dtb root/bin/busybox
