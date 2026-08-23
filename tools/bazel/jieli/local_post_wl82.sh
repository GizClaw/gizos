#!/bin/bash
# Local post-build for JieLi AC791N (wl82): replaces the upstream host-client
# cloud packaging with the tool sequence documented by the SDK's own
# cpu/wl82/tools/download.c (SFC/NOR flash branch).
#
# Usage: local_post_wl82.sh <sdk_root> <toolchain_root> <postbuild_root> <out_dir>
# Publishes firmware.elf, symbols.txt, jl_isd.bin, jl_isd.fw and update.ufw
# into <out_dir>.
set -euo pipefail
sdk=$1
toolchain=$2
postbuild=$3
out=$4
tools=$sdk/cpu/wl82/tools
bin=$toolchain/pi32v2/bin
export QT_QPA_PLATFORM=offscreen

cd "$tools"
test -f sdk.elf
test -f isd_config.ini
# pre_build renders isd_config.ini with clang -E; upstream then runs strip-ini
# to drop the leading echo line and blank lines before isd_download reads it.
sed -i '/^echo /d;/^[[:space:]]*$/d' isd_config.ini

"$bin/objcopy" -O binary -j .text sdk.elf text.bin
"$bin/objcopy" -O binary -j .data sdk.elf data.bin
"$bin/objcopy" -O binary -j .ram0_data sdk.elf ram0_data.bin
"$bin/objcopy" -O binary -j .cache_ram_data sdk.elf cache_ram_data.bin
"$bin/objcopy" -O binary -j .dynamic_data sdk.elf dynamic_data.bin
cat text.bin data.bin dynamic_data.bin ram0_data.bin cache_ram_data.bin > app.bin
"$bin/objdump" -section-headers -address-mask=0x1ffffff sdk.elf > section_headers.txt
"$bin/objsizedump" -lite -skip-zero -enable-dbg-info sdk.elf | sort -k 1 > symbol_tbl.txt

"$postbuild/isd_download" isd_config.ini -gen2 -tonorflash -dev wl82 -boot 0x1c02000 -div1 \
  -wait 300 -uboot uboot.boot -app app.bin cfg_tool.bin -res cfg -reboot 500 -extend-bin \
  || true  # exits non-zero with "Device Offline" when no board is attached
test -s jl_isd.fw
test -s jl_isd.bin
"$postbuild/fw_add" -noenc -fw jl_isd.fw -add ota.bin -type 100 -out jl_isd.fw
"$postbuild/fw_add" -noenc -fw jl_isd.fw -add script.ver -out jl_isd.fw
"$postbuild/ufw_maker" -fw_to_ufw jl_isd.fw
test -s jl_isd.ufw

mkdir -p "$out"
cp sdk.elf "$out/firmware.elf"
cp symbol_tbl.txt "$out/symbols.txt"
cp jl_isd.bin "$out/jl_isd.bin"
cp jl_isd.fw "$out/jl_isd.fw"
cp jl_isd.ufw "$out/update.ufw"
