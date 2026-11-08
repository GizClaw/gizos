#!/bin/bash
# Local post-build for JieLi AC791N (wl82): replaces the upstream host-client
# cloud packaging with the tool sequence documented by the SDK's own
# cpu/wl82/tools/download.c (SFC/NOR flash branch).
#
# Usage: local_post_wl82.sh <sdk_root> <toolchain_root> <postbuild_root> <out_dir>
# Publishes firmware.elf, symbols.txt, jl_isd.bin, jl_isd.fw and the updater's
# managed payload as update.ufw into <out_dir>. In JieLi double-bank mode that
# payload is db_update_files_data.bin; update.ufw is retained as the stable
# Bazel provider filename.
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
# The pinned SDK checkout may contain untracked outputs left by a previous
# vendor build.  The invocation-local SDK copy inherits those files, while
# isd_download is allowed to return non-zero when no board is attached.  Never
# let a stale factory image satisfy the post-build output checks.
rm -f \
  jl_isd.bin \
  jl_isd.fw \
  jl_isd.ufw \
  jl_isd_extend.bin \
  db_update_files_data.bin \
  db_update_files_data.managed.bin
# pre_build renders isd_config.ini with clang -E; upstream then runs strip-ini
# to drop the leading echo line and blank lines before isd_download reads it.
sed -i.bak '/^echo /d;/^[[:space:]]*$/d' isd_config.ini
rm -f isd_config.ini.bak

double_bank=0
if grep -q '^BR22_TWS_DB=YES;' isd_config.ini; then
  double_bank=1
fi

"$bin/objcopy" -O binary -j .text sdk.elf text.bin
"$bin/objcopy" -O binary -j .data sdk.elf data.bin
"$bin/objcopy" -O binary -j .ram0_data sdk.elf ram0_data.bin
"$bin/objcopy" -O binary -j .cache_ram_data sdk.elf cache_ram_data.bin
"$bin/objcopy" -O binary -j .dynamic_data sdk.elf dynamic_data.bin
cat text.bin data.bin dynamic_data.bin ram0_data.bin cache_ram_data.bin > app.bin
"$bin/objdump" -section-headers -address-mask=0x1ffffff sdk.elf > section_headers.txt
"$bin/objsizedump" -lite -skip-zero -enable-dbg-info sdk.elf | sort -k 1 > symbol_tbl.txt

if (( double_bank )); then
  "$postbuild/isd_download" isd_config.ini -gen2 -tonorflash -dev wl82 \
    -boot 0x1c02000 -div1 -wait 300 -uboot uboot.boot \
    -app app.bin cfg_tool.bin -res cfg -reboot 500 \
    -update_files normal -extend-bin \
    || true  # exits non-zero with "Device Offline" when no board is attached
  test -s db_update_files_data.bin
  cp db_update_files_data.bin db_update_files_data.managed.bin
else
  "$postbuild/isd_download" isd_config.ini -gen2 -tonorflash -dev wl82 \
    -boot 0x1c02000 -div1 -wait 300 -uboot uboot.boot \
    -app app.bin cfg_tool.bin -res cfg -reboot 500 -extend-bin \
    || true  # exits non-zero with "Device Offline" when no board is attached
fi
test -s jl_isd.fw
test -s jl_isd.bin
test -s jl_isd_extend.bin
if (( double_bank )); then
  test -s db_update_files_data.managed.bin
  managed_update=db_update_files_data.managed.bin
else
  "$postbuild/fw_add" -noenc -fw jl_isd.fw -add ota.bin -type 100 -out jl_isd.fw
  managed_update=jl_isd.ufw
fi
"$postbuild/fw_add" -noenc -fw jl_isd.fw -add script.ver -out jl_isd.fw
"$postbuild/ufw_maker" -fw_to_ufw jl_isd.fw
test -s jl_isd.ufw

mkdir -p "$out"
cp sdk.elf "$out/firmware.elf"
cp symbol_tbl.txt "$out/symbols.txt"
# `jl_isd.bin` is the compact download image.  It omits the erased gaps and
# tail regions required after a whole-chip erase; publish the image generated
# by `-extend-bin` as the Bazel factory/flash artifact instead.
cp jl_isd_extend.bin "$out/jl_isd.bin"
cp jl_isd.fw "$out/jl_isd.fw"
cp "$managed_update" "$out/update.ufw"
