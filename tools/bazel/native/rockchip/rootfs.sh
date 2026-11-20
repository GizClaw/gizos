rootfs=$1 archive=$2 config=$3 inittab=$4 rc_sysinit=$5 rkbin_license=$6
tar -xf "$archive" --strip-components=1
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCONFIG_NOTIMESTAMP=1
make -s allnoconfig > /dev/null
# BusyBox's allnoconfig resets even values in KCONFIG_ALLCONFIG. Override its
# generated baseline and let oldconfig resolve each explicitly selected applet.
while IFS='=' read -r key value; do
    if [[ $key == CONFIG_* ]]; then
        sed -i -E "/^(# )?${key}(=| is not set$)/d" .config
        printf '%s=%s\n' "$key" "$value" >> .config
    fi
done < "$config"
scripts/kconfig/conf -D .config Config.in > /dev/null
while IFS= read -r setting; do
    [[ $setting != CONFIG_*=* ]] || grep -Fqx "$setting" .config || { echo "Missing BusyBox setting: $setting" >&2; exit 1; }
done < "$config"
make -s -j"$jobs" busybox
root=$PWD/root
make -s CONFIG_PREFIX="$root" install
mkdir -p "$root"/{dev,etc,proc,sys,run,tmp,usr/share/licenses}
install -m 0644 "$inittab" "$root/etc/inittab"
install -m 0755 "$rc_sysinit" "$root/etc/rcS"
install -m 0644 LICENSE "$root/usr/share/licenses/BusyBox"
install -m 0644 "$rkbin_license" "$root/usr/share/licenses/rockchip-rkbin"
# Explicit timestamps also cover symlinks and the SquashFS superblock.
epoch=$SOURCE_DATE_EPOCH
unset SOURCE_DATE_EPOCH
mksquashfs "$root" "$rootfs" -comp gzip -all-root -all-time "$epoch" -mkfs-time "$epoch" -noappend -no-xattrs -no-progress -processors 1
