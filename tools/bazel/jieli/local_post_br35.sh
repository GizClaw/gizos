#!/bin/bash
# AC707N bare-chip packaging, using the BR35 SDK section order and boot assets.
# Run only in the invocation-local SDK copy supplied by jieli_runner.py.
set -euo pipefail
sdk=$1
toolchain=$2
postbuild=$3
out=$4
bin=$toolchain/pi32v2/bin
cd "$sdk/cpu/br35/tools"
export QT_QPA_PLATFORM=offscreen
for section in text data data_code overlay_aec overlay_aac ps_ram_data_code dcache_ram_data icache_ram_data_code; do
  "$bin/objcopy" -O binary -j ".$section" sdk.elf "$section.bin"
done
cat text.bin data.bin data_code.bin overlay_aec.bin overlay_aac.bin ps_ram_data_code.bin dcache_ram_data.bin icache_ram_data_code.bin > app.bin
"$bin/objsizedump" -lite -skip-zero -enable-dbg-info sdk.elf | sort -k 1 > symbol_tbl.txt
# Delete any SDK-shipped outputs so an unsuccessful packager cannot pass using
# an old image. No application UI/audio resources are enabled by this layout.
rm -f jl_isd.bin jl_isd.fw update.ufw
"$postbuild/isd_download" isd_config.ini -gen2 -tonorflash -dev br35 \
  -boot 0x102600 -div8 -wait 300 -uboot uboot.boot -app app.bin \
  -res p11_code.bin -flash-params flash_params_v3.bin \
  -output-fw jl_isd.fw -output-ufw update.ufw > packaging.log 2>&1 || true
cat packaging.log
# The packager may still emit files when an included OTA loader does not fit.
if grep -q "!!!!!! FAIL:" packaging.log; then
  echo "BR35 OTA loader capacity check failed" >&2
  exit 1
fi
# isd_download may return Device Offline after successfully generating files.
test -s jl_isd.bin
test -s jl_isd.fw
test -s update.ufw
mkdir -p "$out"
cp sdk.elf "$out/firmware.elf"
cp symbol_tbl.txt "$out/symbols.txt"
cp jl_isd.bin jl_isd.fw update.ufw "$out/"
