#!/bin/bash
# Local post-build for JieLi AC695N (br23): replaces the upstream host-client
# cloud packaging with the tool sequence documented by the SDK's own
# cpu/br23/tools/download.c and download/standard/download.bat.
#
# Usage: local_post_br23.sh <sdk_root> <toolchain_root> <postbuild_root> <out_dir>
# Publishes firmware.elf, symbols.txt, jl_isd.bin, jl_isd.fw and update.ufw
# into <out_dir>; nor_update.ufw is left next to them for manual use.
set -euo pipefail
sdk=$1
toolchain=$2
postbuild=$3
out=$4
tools=$sdk/cpu/br23/tools
bin=$toolchain/pi32v2/bin
export QT_QPA_PLATFORM=offscreen

cd "$tools"
test -f sdk.elf
"$bin/objcopy" -O binary -j .text sdk.elf text.bin
"$bin/objcopy" -O binary -j .data sdk.elf data.bin
"$bin/objcopy" -O binary -j .data_code sdk.elf data_code.bin
"$bin/objcopy" -O binary -j .overlay_aec sdk.elf aeco.bin
"$bin/objcopy" -O binary -j .overlay_wav sdk.elf wav.bin
"$bin/objcopy" -O binary -j .overlay_ape sdk.elf ape.bin
"$bin/objcopy" -O binary -j .overlay_flac sdk.elf flac.bin
"$bin/objcopy" -O binary -j .overlay_m4a sdk.elf m4a.bin
"$bin/objcopy" -O binary -j .overlay_amr sdk.elf amr.bin
"$bin/objcopy" -O binary -j .overlay_dts sdk.elf dts.bin
"$bin/objcopy" -O binary -j .overlay_fm sdk.elf fmo.bin
"$bin/objcopy" -O binary -j .overlay_mp3 sdk.elf mp3o.bin
"$bin/objcopy" -O binary -j .overlay_wma sdk.elf wmao.bin
"$postbuild/remove_tailing_zeros" -i aeco.bin -o aec.bin -mark ff
"$postbuild/remove_tailing_zeros" -i fmo.bin -o fm.bin -mark ff
"$postbuild/remove_tailing_zeros" -i mp3o.bin -o mp3.bin -mark ff
"$postbuild/remove_tailing_zeros" -i wmao.bin -o wma.bin -mark ff
"$bin/objdump" -section-headers -address-mask=0x1ffffff sdk.elf > section_headers.txt
"$bin/objsizedump" -lite -skip-zero -enable-dbg-info sdk.elf | sort -k 1 > symbol_tbl.txt
# Upstream download.c selects the EQ effect file from app_config; the
# soundbox default is music_base.bin.
cp effect_file/music_base.bin eq_cfg_hw.bin
# bank.bin only exists when the link produces banked code.
[ -f bank.bin ] || : > bank.bin
cat text.bin data.bin data_code.bin aec.bin wav.bin ape.bin flac.bin m4a.bin amr.bin dts.bin fm.bin mp3.bin wma.bin bank.bin > app.bin

# download/standard/download.bat, run in a scratch copy of that directory.
work=$(mktemp -d)
cp -a download/standard/. "$work"/
cp script.ver uboot.boot tone.cfg cfg_tool.bin app.bin br23loader.bin eq_cfg_hw.bin ota_all.bin ota_nor.bin "$work"/
cd "$work"
"$postbuild/isd_download" isd_config.ini -tonorflash -dev br23 -boot 0x12000 -div8 -wait 300 \
  -uboot uboot.boot -app app.bin -res tone.cfg cfg_tool.bin eq_cfg_hw.bin -format all -key AC69XX.key \
  || true  # exits non-zero with "Device Offline" when no board is attached
test -s jl_isd.fw
test -s jl_isd.bin
cp ota_all.bin ota.bin
"$postbuild/fw_add" -noenc -fw jl_isd.fw -add ota.bin -type 100 -out jl_isd_all.fw
cp ota_nor.bin ota.bin
"$postbuild/fw_add" -noenc -fw jl_isd.fw -add ota.bin -type 100 -out jl_isd_nor.fw
"$postbuild/fw_add" -noenc -fw jl_isd_all.fw -add script.ver -out jl_isd_all.fw
"$postbuild/fw_add" -noenc -fw jl_isd_nor.fw -add script.ver -out jl_isd_nor.fw
"$postbuild/ufw_maker" -fw_to_ufw jl_isd_all.fw
"$postbuild/ufw_maker" -fw_to_ufw jl_isd_nor.fw

mkdir -p "$out"
cp "$tools/sdk.elf" "$out/firmware.elf"
cp "$tools/symbol_tbl.txt" "$out/symbols.txt"
cp jl_isd.bin "$out/jl_isd.bin"
cp jl_isd_all.fw "$out/jl_isd.fw"
cp jl_isd_all.ufw "$out/update.ufw"
cp jl_isd_nor.ufw "$out/nor_update.ufw"
rm -rf "$work"
