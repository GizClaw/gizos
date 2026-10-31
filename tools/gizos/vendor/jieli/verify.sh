#!/usr/bin/env sh
# Verify the unpacked JieLi toolchain and post-build tool trees.
# Usage: verify.sh [toolchain_root [postbuild_root]]
# Environment: JIELI_TOOLCHAIN_ROOT, JIELI_POSTBUILD_ROOT override the defaults.
# JIELI_VERIFY_RUN=0 skips executing clang on Linux x86_64 hosts.
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
repo_root=$(CDPATH='' cd -- "$script_dir/../.." && pwd -P)
toolchain_version=20250805.1
postbuild_version=20260728.1
expected_clang_version=4.0.1
toolchain_root=${1:-${JIELI_TOOLCHAIN_ROOT:-$repo_root/.tools/jieli-linux-toolchains-$toolchain_version}}
postbuild_root=${2:-${JIELI_POSTBUILD_ROOT:-$repo_root/.tools/jieli-linux-post-build-tools-$postbuild_version}}
host_os=${JIELI_HOST_OS:-$(uname -s)}
host_arch=${JIELI_HOST_ARCH:-$(uname -m)}

require_file() {
    if [ ! -f "$1" ]; then
        printf 'gizos: JieLi %s input is missing: %s\n' "$2" "$1" >&2
        exit 2
    fi
}

require_executable() {
    if [ ! -x "$1" ]; then
        printf 'gizos: JieLi %s tool is unavailable: %s\n' "$2" "$1" >&2
        exit 2
    fi
}

require_version() {
    marker=$1/.firmware-devenv-version
    require_file "$marker" "$2"
    actual=$(cat "$marker")
    if [ "$actual" != "$3" ]; then
        printf 'gizos: JieLi %s version mismatch: expected %s, found %s\n' \
            "$2" "$3" "${actual:-unknown}" >&2
        exit 2
    fi
}

# Toolchain: one private LLVM/Clang tree per core family plus shared helpers.
require_version "$toolchain_root" toolchain "$toolchain_version"
require_file "$toolchain_root/README.md" toolchain
for core in pi32 pi32v2 q32s; do
    for tool in clang ld lto-wrapper; do
        require_executable "$toolchain_root/$core/bin/$tool" toolchain
    done
done
require_executable "$toolchain_root/common/bin/clang" toolchain

# Post-build: flat Linux x86_64 binaries used by the SDK post_build scripts.
require_version "$postbuild_root" post-build "$postbuild_version"
for tool in isd_download fw_add ufw_maker pack_res json_to_res fat_comm \
        remove_tailing_zeros; do
    require_executable "$postbuild_root/$tool" post-build
done

if [ "$host_os-$host_arch" = Linux-x86_64 ] && [ "${JIELI_VERIFY_RUN:-1}" != 0 ]; then
    clang_version=$("$toolchain_root/pi32v2/bin/clang" --version 2>/dev/null | sed -n '1p') || {
        printf 'gizos: JieLi pi32v2 clang cannot run: %s\n' \
            "$toolchain_root/pi32v2/bin/clang" >&2
        exit 2
    }
    case "$clang_version" in
        "clang version $expected_clang_version "*|"clang version $expected_clang_version") ;;
        *)
            printf 'gizos: JieLi pi32v2 clang mismatch: expected clang version %s, found %s\n' \
                "$expected_clang_version" "${clang_version:-unknown}" >&2
            exit 2
            ;;
    esac
fi

printf 'JieLi toolchain %s: %s\n' "$toolchain_version" "$toolchain_root"
printf 'JieLi post-build tools %s: %s\n' "$postbuild_version" "$postbuild_root"
