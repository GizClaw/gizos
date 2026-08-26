#!/usr/bin/env bash

set -euo pipefail

repository_root=$(git rev-parse --show-toplevel)
fixture_root="$repository_root/tools/bazel/tests/downstream_consumer"
consumer_root=$(mktemp -d "${TMPDIR:-/tmp}/gizos-downstream-consumer.XXXXXX")

cleanup() {
    "${BAZEL_BIN:-bazel}" --output_base="$consumer_root/output-base" shutdown >/dev/null 2>&1 || true
    chmod -R u+w "$consumer_root" 2>/dev/null || true
    rm -rf "$consumer_root"
}
trap cleanup EXIT
repository_cache=${BAZEL_REPOSITORY_CACHE:-"$HOME/.cache/bazel/repository"}
mkdir -p "$repository_cache"

cp "$fixture_root/MODULE.bazel.fixture" "$consumer_root/MODULE.bazel"
cp "$fixture_root/BUILD.bazel.fixture" "$consumer_root/BUILD.bazel"
mkdir -p "$consumer_root/projects/consumer/targets/h2loader_tar_zlib/main/consumer_board"
cp "$fixture_root/package.BUILD.bazel.fixture" \
    "$consumer_root/projects/consumer/targets/h2loader_tar_zlib/main/consumer_board/BUILD.bazel"
mkdir -p "$consumer_root/projects/consumer/targets/bk3633_firmware/main/consumer_board"
cp "$fixture_root/bk3633.BUILD.bazel.fixture" \
    "$consumer_root/projects/consumer/targets/bk3633_firmware/main/consumer_board/BUILD.bazel"
mkdir -p "$consumer_root/bk3633"
cp "$fixture_root/bk3633.Makefile.fixture" "$consumer_root/bk3633/Makefile"
cp "$fixture_root/release_identity.cquery.fixture" "$consumer_root/release_identity.cquery"
cp "$fixture_root/consumer.bzl.fixture" "$consumer_root/consumer.bzl"
cp "$fixture_root/consumer.c" "$consumer_root/consumer.c"
cp "$fixture_root/CMakeLists.txt.fixture" "$consumer_root/CMakeLists.txt"
cp "$fixture_root/ap.defaults" "$consumer_root/ap.defaults"
cp "$fixture_root/ap_gpio.h" "$consumer_root/ap_gpio.h"
cp "$fixture_root/cp.defaults" "$consumer_root/cp.defaults"
cp "$fixture_root/cp_gpio.h" "$consumer_root/cp_gpio.h"
cp "$fixture_root/disabled_native_locator.json" "$consumer_root/disabled_native_locator.json"
cp "$fixture_root/layout.txt" "$consumer_root/layout.txt"
cp "$fixture_root/partition.csv" "$consumer_root/partition.csv"
cp "$fixture_root/ram_regions.csv" "$consumer_root/ram_regions.csv"
cp "$fixture_root/sdkconfig.h2loader.defaults" "$consumer_root/sdkconfig.h2loader.defaults"
for policy in private_esp_task_policy private_bk_ap_task_policy private_bk_cp_task_policy; do
    mkdir -p "$consumer_root/$policy"
    cp "$fixture_root/$policy.c" "$consumer_root/$policy/$policy.c"
    cp "$fixture_root/$policy.h" "$consumer_root/$policy/$policy.h"
    cp "$fixture_root/$policy.CMakeLists.txt.fixture" \
        "$consumer_root/$policy/CMakeLists.txt"
done

case "$(uname -s)-$(uname -m)" in
    Darwin-arm64)
        host_os=macos
        platform=macos_arm64
        ;;
    Linux-x86_64)
        host_os=linux
        platform=linux_x86_64
        ;;
    Linux-aarch64 | Linux-arm64)
        host_os=linux
        platform=linux_arm64
        ;;
    *)
        printf 'unsupported downstream-consumer host: %s-%s\n' "$(uname -s)" "$(uname -m)" >&2
        exit 2
        ;;
esac

cd "$consumer_root"

"${BAZEL_BIN:-bazel}" \
    --ignore_all_rc_files \
    --output_base="$consumer_root/output-base" \
    cquery \
    --enable_bzlmod \
    --noenable_workspace \
    --repository_cache="$repository_cache" \
    --override_module="gizos=$repository_root" \
    --define="h2_ci_graph=true" \
    --define="h2_host_os=$host_os" \
    --platforms="@gizos//tools/bazel/platforms:$platform" \
    'set(//:private_bk_firmware //:private_esp_firmware //projects/consumer/targets/bk3633_firmware/main/consumer_board:firmware)'

cp BUILD.bazel BUILD.bazel.complete
expect_missing_policy_failure() {
    local field=$1
    local target=$2
    cp BUILD.bazel.complete BUILD.bazel
    sed -i.bak "/\"${field}\":/d" BUILD.bazel
    if "${BAZEL_BIN:-bazel}" \
        --ignore_all_rc_files \
        --output_base="$consumer_root/output-base" \
        cquery \
        --enable_bzlmod \
        --noenable_workspace \
        --repository_cache="$repository_cache" \
        --override_module="gizos=$repository_root" \
        --define="h2_ci_graph=true" \
        --define="h2_host_os=$host_os" \
        --platforms="@gizos//tools/bazel/platforms:$platform" \
        "$target" >"missing-${field}.log" 2>&1; then
        printf 'expected missing %s to fail analysis\n' "$field" >&2
        exit 1
    fi
    grep -F "missing ${field}" "missing-${field}.log" >/dev/null
    grep -F "private_" "missing-${field}.log" >/dev/null
}

expect_missing_policy_failure task_policy //:private_esp_firmware
expect_missing_policy_failure ap_task_policy //:private_bk_firmware
expect_missing_policy_failure cp_task_policy //:private_bk_firmware
cp BUILD.bazel.complete BUILD.bazel

"${BAZEL_BIN:-bazel}" \
    --ignore_all_rc_files \
    --output_base="$consumer_root/output-base" \
    build \
    --enable_bzlmod \
    --noenable_workspace \
    --repository_cache="$repository_cache" \
    --override_module="gizos=$repository_root" \
    --define="h2_host_os=$host_os" \
    --platforms="@gizos//tools/bazel/platforms:$platform" \
    --extra_toolchains="@gizos//tools/bazel/platforms:${platform}_test_toolchain" \
    @gizos//tools/openapi_codegen:openapi_codegen \
    //:bk3633_test_support_consumer \
    //:firmware_lib \
    //:i18n_runtime \
    //:native_component \
    //projects/consumer/targets/h2loader_tar_zlib/main/consumer_board:package \
    //:runtime

package_files=$("${BAZEL_BIN:-bazel}" \
    --ignore_all_rc_files \
    --output_base="$consumer_root/output-base" \
    cquery \
    --enable_bzlmod \
    --noenable_workspace \
    --repository_cache="$repository_cache" \
    --override_module="gizos=$repository_root" \
    --define="h2_host_os=$host_os" \
    --platforms="@gizos//tools/bazel/platforms:$platform" \
    --extra_toolchains="@gizos//tools/bazel/platforms:${platform}_test_toolchain" \
    --output=files \
    //projects/consumer/targets/h2loader_tar_zlib/main/consumer_board:package)
grep -F '/consumer-main-consumer_board.update.tar.zlib' <<<"$package_files" >/dev/null
grep -F '/consumer-main-consumer_board.firmware.json' <<<"$package_files" >/dev/null
package_identity=$("${BAZEL_BIN:-bazel}" \
    --ignore_all_rc_files \
    --output_base="$consumer_root/output-base" \
    cquery \
    --enable_bzlmod \
    --noenable_workspace \
    --repository_cache="$repository_cache" \
    --override_module="gizos=$repository_root" \
    --define="h2_host_os=$host_os" \
    --platforms="@gizos//tools/bazel/platforms:$platform" \
    --extra_toolchains="@gizos//tools/bazel/platforms:${platform}_test_toolchain" \
    --output=starlark \
    --starlark:file="$consumer_root/release_identity.cquery" \
    //projects/consumer/targets/h2loader_tar_zlib/main/consumer_board:package)
test "$package_identity" = 'consumer|main|consumer_board|consumer-main-consumer_board.update.tar.zlib'

bk3633_files=$("${BAZEL_BIN:-bazel}" \
    --ignore_all_rc_files \
    --output_base="$consumer_root/output-base" \
    cquery \
    --enable_bzlmod \
    --noenable_workspace \
    --repository_cache="$repository_cache" \
    --override_module="gizos=$repository_root" \
    --define="h2_ci_graph=true" \
    --define="h2_host_os=$host_os" \
    --platforms="@gizos//tools/bazel/platforms:$platform" \
    --output=files \
    //projects/consumer/targets/bk3633_firmware/main/consumer_board:firmware)
grep -F '/consumer-main-consumer_board.bin' <<<"$bk3633_files" >/dev/null
grep -F '/app.bin' <<<"$bk3633_files" >/dev/null
bk3633_identity=$("${BAZEL_BIN:-bazel}" \
    --ignore_all_rc_files \
    --output_base="$consumer_root/output-base" \
    cquery \
    --enable_bzlmod \
    --noenable_workspace \
    --repository_cache="$repository_cache" \
    --override_module="gizos=$repository_root" \
    --define="h2_ci_graph=true" \
    --define="h2_host_os=$host_os" \
    --platforms="@gizos//tools/bazel/platforms:$platform" \
    --output=starlark \
    --starlark:file="$consumer_root/release_identity.cquery" \
    //projects/consumer/targets/bk3633_firmware/main/consumer_board:firmware)
test "$bk3633_identity" = 'consumer|main|consumer_board|consumer-main-consumer_board.bin'

"${BAZEL_BIN:-bazel}" \
    --ignore_all_rc_files \
    --output_base="$consumer_root/output-base" \
    build \
    --enable_bzlmod \
    --noenable_workspace \
    --repository_cache="$repository_cache" \
    --override_module="gizos=$repository_root" \
    --define="h2_firmware_target=bk3633" \
    --define="h2_host_os=$host_os" \
    --platforms="@gizos//tools/bazel/platforms:bk3633" \
    //:bk3633_wolfcrypt_consumer
