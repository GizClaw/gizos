image=$1 dtb=$2 archive=$3 config=$4
shift 4
tar -xf "$archive" --strip-components=1
for upstream_patch do patch --batch --fuzz=0 -p1 < "$upstream_patch"; done
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LOCALVERSION=
cp "$config" .config
make -s olddefconfig
# Catch silently dropped Kconfig options, in particular drivers needed before init.
while IFS= read -r setting; do
    [[ $setting != CONFIG_*=* ]] || grep -Fqx "$setting" .config || { echo "Missing kernel setting: $setting" >&2; exit 1; }
done < "$config"
board=rockchip/rk3326-gameconsole-r36s.dtb
make -s -j"$jobs" Image "$board"
cp arch/arm64/boot/Image "$image"
cp "arch/arm64/boot/dts/$board" "$dtb"
