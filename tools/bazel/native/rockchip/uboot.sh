# ROCKNIX's legacy ('a') loader for genuine RK3326 handhelds.
idb=$1 uboot=$2 trust=$3 boot_scr=$4 archive=$5 fix=$6 config=$7 ddr=$8 miniloader=$9 bl31=${10} boot_cmd=${11}
tar -xf "$archive" --strip-components=1
patch --batch --fuzz=0 -p1 < "$fix"
cp "$config" configs/rocknix_rk3326_defconfig
export ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- LOCALVERSION=
make -s rocknix_rk3326_defconfig
make -s -j"$jobs" HOSTCFLAGS='-O2 -fcommon' CONFIG_MKIMAGE_DTC_PATH=scripts/dtc/dtc u-boot-dtb.bin tools
# RK3326 ROM reads DDR from the Rockchip ID block and then the appended miniloader.
tools/mkimage -n px30 -T rksd -d "$ddr" -C bzip2 "$idb"
cat "$miniloader" >> "$idb"
tools/loaderimage --pack --uboot u-boot-dtb.bin uboot.img 0x00200000
cp uboot.img "$uboot"
# The legacy packer has fixed-size path fields. Keep its input and output names short.
cp "$bl31" bl31.elf
cat > trust.ini <<'EOF'
[BL30_OPTION]
SEC=0
[BL31_OPTION]
SEC=1
PATH=bl31.elf
ADDR=0x00010000
[BL32_OPTION]
SEC=0
[BL33_OPTION]
SEC=0
[OUTPUT]
PATH=trust.img
EOF
tools/trust_merger trust.ini
cp trust.img "$trust"
tools/mkimage -A arm64 -O linux -T script -C none -n 'GizOS R36S Panel 0' -d "$boot_cmd" "$boot_scr"
