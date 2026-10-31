#!/usr/bin/env sh
# Unpack the LFS-mirrored JieLi toolchain and post-build tarballs into .tools/.
# No network access: archives/ is the only source, verified against SHA256SUMS.
# Environment: JIELI_TOOLS_DIR (default .tools), JIELI_ARCHIVE_DIR (default
# tools/jieli_toolchain/archives).
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
repo_root=$(CDPATH='' cd -- "$script_dir/../.." && pwd -P)
tools_dir=${JIELI_TOOLS_DIR:-$repo_root/.tools}
archive_dir=${JIELI_ARCHIVE_DIR:-$script_dir/archives}
toolchain_version=20250805.1
postbuild_version=20260728.1
toolchain_name=jieli-linux-toolchains-$toolchain_version
postbuild_name=jieli-linux-post-build-tools-$postbuild_version
sums_file=$archive_dir/SHA256SUMS

if command -v shasum >/dev/null 2>&1; then
    sha256() { shasum -a 256 "$1" | awk '{print $1}'; }
elif command -v sha256sum >/dev/null 2>&1; then
    sha256() { sha256sum "$1" | awk '{print $1}'; }
else
    printf '%s\n' 'gizos: shasum or sha256sum is required' >&2
    exit 2
fi

expected_sha256() {
    awk -v name="$1" '$2 == name || $2 == "*" name {print $1; exit}' "$sums_file"
}

check_archive() {
    archive_path=$archive_dir/$1
    if [ ! -f "$archive_path" ]; then
        printf 'gizos: JieLi archive is missing: %s\n' "$archive_path" >&2
        exit 2
    fi
    if head -c 64 "$archive_path" | grep -q '^version https://git-lfs.github.com/spec/'; then
        printf 'gizos: JieLi archive is an unfetched Git LFS pointer: %s; materialize it in your authorized asset repository first\n' \
            "$archive_path" >&2
        exit 2
    fi
    expected=$(expected_sha256 "$1")
    if [ -z "$expected" ]; then
        printf 'gizos: JieLi archive has no SHA256SUMS entry: %s\n' "$1" >&2
        exit 2
    fi
    actual=$(sha256 "$archive_path")
    if [ "$actual" != "$expected" ]; then
        printf 'JieLi archive checksum mismatch for %s: expected %s, got %s\n' \
            "$1" "$expected" "$actual" >&2
        exit 1
    fi
}

# install_component <name> <archive> <version> <post-extract hook>
# Extracts with --strip-components=1 so the install directory is named after
# the archive version even when the upstream tarball carries a stale inner
# directory name, then swaps it into place atomically.
install_component() {
    name=$1
    archive=$2
    version=$3
    hook=$4
    install_dir=$tools_dir/$name
    case "$install_dir" in
        */"$name") ;;
        *)
            printf 'gizos: refusing unsafe JieLi install path: %s\n' "$install_dir" >&2
            exit 2
            ;;
    esac

    check_archive "$archive"
    mkdir -p "$tools_dir"
    tmp_root=$tools_dir/.$name.tmp.$$
    backup_root=$tools_dir/.$name.backup.$$
    rm -rf "$tmp_root" "$backup_root"
    trap 'rm -rf "$tmp_root" "$backup_root"' EXIT HUP INT TERM

    mkdir -p "$tmp_root"
    tar -xJf "$archive_dir/$archive" -C "$tmp_root" --strip-components=1
    "$hook" "$tmp_root"
    printf '%s\n' "$version" >"$tmp_root/.firmware-devenv-version"

    if [ -e "$install_dir" ]; then
        mv "$install_dir" "$backup_root"
    fi
    if mv "$tmp_root" "$install_dir"; then
        rm -rf "$backup_root"
    else
        [ ! -e "$backup_root" ] || mv "$backup_root" "$install_dir"
        exit 1
    fi
    trap - EXIT HUP INT TERM
}

toolchain_hook() {
    :
}

postbuild_hook() {
    # The SDK post_build scripts invoke pack_res; upstream ships it as packres.
    if [ -f "$1/packres" ] && [ ! -e "$1/pack_res" ]; then
        mv "$1/packres" "$1/pack_res"
    fi
    chmod +x "$1"/*
}

toolchain_root=$tools_dir/$toolchain_name
postbuild_root=$tools_dir/$postbuild_name

component_installed() {
    [ "$(cat "$1/.firmware-devenv-version" 2>/dev/null)" = "$2" ] && [ -x "$1/$3" ]
}

if ! component_installed "$toolchain_root" "$toolchain_version" pi32v2/bin/clang; then
    install_component "$toolchain_name" "$toolchain_name.tar.xz" \
        "$toolchain_version" toolchain_hook
fi
if ! component_installed "$postbuild_root" "$postbuild_version" isd_download; then
    install_component "$postbuild_name" "$postbuild_name.tar.xz" \
        "$postbuild_version" postbuild_hook
fi
JIELI_VERIFY_RUN=0 "$script_dir/verify.sh" "$toolchain_root" "$postbuild_root" >/dev/null

printf '%s\n' "$toolchain_root" "$postbuild_root"
