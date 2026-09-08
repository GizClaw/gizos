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
cp "$fixture_root/consumer.bzl.fixture" "$consumer_root/consumer.bzl"
cp "$fixture_root/consumer.c" "$consumer_root/consumer.c"
cp "$fixture_root/CMakeLists.txt.fixture" "$consumer_root/CMakeLists.txt"
cp "$fixture_root/ap.defaults" "$consumer_root/ap.defaults"
cp "$fixture_root/ap_gpio.h" "$consumer_root/ap_gpio.h"
cp "$fixture_root/cp.defaults" "$consumer_root/cp.defaults"
cp "$fixture_root/cp_gpio.h" "$consumer_root/cp_gpio.h"
cp "$fixture_root/disabled_native_locator.json" "$consumer_root/disabled_native_locator.json"
cp "$fixture_root/embedded_ttf_consumer.cc" "$consumer_root/embedded_ttf_consumer.cc"
cp "$fixture_root/font_consumer.c" "$consumer_root/font_consumer.c"
cp "$fixture_root/font_consumer_test.c" "$consumer_root/font_consumer_test.c"
cp "$fixture_root/font_symbols.txt" "$consumer_root/font_symbols.txt"
cp "$repository_root/projects/showcase/assets/fonts/NotoSansSC-Bold.ttf" \
    "$consumer_root/font.ttf"
cp "$fixture_root/layout.txt" "$consumer_root/layout.txt"
cp "$fixture_root/partition.csv" "$consumer_root/partition.csv"
cp "$fixture_root/ram_regions.csv" "$consumer_root/ram_regions.csv"
cp "$fixture_root/sdkconfig.h2loader.defaults" "$consumer_root/sdkconfig.h2loader.defaults"
# The public rules must work without any consumer-owned policy directory.
test ! -e "$consumer_root/private_esp_task_policy"
test ! -e "$consumer_root/private_bk_task_policy"

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
    'set(//:generic_private_bk_firmware //:generic_private_esp_firmware //:private_bk_firmware //:private_esp_firmware //:private_bk_ap_task_policy_test //:private_bk_cp_task_policy_test //:private_esp_task_policy_test)'

"${BAZEL_BIN:-bazel}" \
    --ignore_all_rc_files \
    --output_base="$consumer_root/output-base" \
    aquery \
    --enable_bzlmod \
    --noenable_workspace \
    --repository_cache="$repository_cache" \
    --override_module="gizos=$repository_root" \
    --define="h2_ci_graph=true" \
    --define="h2_host_os=$host_os" \
    --platforms="@gizos//tools/bazel/platforms:$platform" \
    'mnemonic(EspIdfFirmware, //:generic_private_esp_firmware)' \
    >font-native-staging.log
grep -F -- '--native-component' font-native-staging.log >/dev/null
grep -F 'embedded_menu_font=.' font-native-staging.log >/dev/null
grep -F -- '--native-component-source' font-native-staging.log >/dev/null
grep -F 'embedded_menu_font.c' font-native-staging.log >/dev/null
grep -F 'embedded_menu_font.h' font-native-staging.log >/dev/null
# The descriptor must point at generated metadata, not a nonexistent source directory.
grep -E 'h2_esp_target_task_policy=bazel-out/.*/private_esp_task_policy' font-native-staging.log >/dev/null


"${BAZEL_BIN:-bazel}" \
    --ignore_all_rc_files \
    --output_base="$consumer_root/output-base" \
    aquery \
    --enable_bzlmod \
    --noenable_workspace \
    --repository_cache="$repository_cache" \
    --override_module="gizos=$repository_root" \
    --define="h2_ci_graph=true" \
    --define="h2_host_os=$host_os" \
    --platforms="@gizos//tools/bazel/platforms:$platform" \
    'mnemonic(Bk7258Firmware, //:generic_private_bk_firmware)' \
    >bk-policy-staging.log
for unit in ap cp; do
    for file in CMakeLists.txt h2_bk_target_task_policy.h h2_bk_target_task_policy.c; do
        grep -E "${unit}:h2_bk_target_task_policy=bazel-out/.*/private_bk_task_policy/${unit}/${file}" bk-policy-staging.log >/dev/null
    done
done

cp BUILD.bazel BUILD.bazel.complete
expect_missing_policy_failure() {
    local field=$1
    local target=$2
    cp BUILD.bazel.complete BUILD.bazel
    sed -i.bak "/^[[:space:]]*${field} =/d" BUILD.bazel
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

# Generating the component shell must not disconnect the graph coverage audit.
sed -i.bak 's/tasks = \["private\/consumer"\]/tasks = ["private\/consumer", "private\/unconfigured"]/' BUILD.bazel
for target in private_esp_firmware private_bk_firmware; do
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
        "//:$target" >"unconfigured-${target}.log" 2>&1; then
        printf 'expected uncovered task to fail analysis: %s\n' "$target" >&2
        exit 1
    fi
    grep -F 'private/unconfigured' "unconfigured-${target}.log" >/dev/null
done
cp BUILD.bazel.complete BUILD.bazel

sed -i.bak 's/font_name = "downstream_font_16"/font_name = "9invalid"/' BUILD.bazel
if "${BAZEL_BIN:-bazel}" \
    --ignore_all_rc_files \
    --output_base="$consumer_root/output-base" \
    cquery \
    --enable_bzlmod \
    --noenable_workspace \
    --repository_cache="$repository_cache" \
    --override_module="gizos=$repository_root" \
    --define="h2_host_os=$host_os" \
    --platforms="@gizos//tools/bazel/platforms:$platform" \
    //:downstream_font_16 >invalid-font-name.log 2>&1; then
    printf 'expected invalid LVGL font symbol name to fail analysis\n' >&2
    exit 1
fi
grep -F "font_name must be a valid C identifier" invalid-font-name.log >/dev/null
cp BUILD.bazel.complete BUILD.bazel

sed -i.bak 's/symbol = "embedded_menu_font"/symbol = "9invalid"/' BUILD.bazel
if "${BAZEL_BIN:-bazel}" \
    --ignore_all_rc_files \
    --output_base="$consumer_root/output-base" \
    cquery \
    --enable_bzlmod \
    --noenable_workspace \
    --repository_cache="$repository_cache" \
    --override_module="gizos=$repository_root" \
    --define="h2_host_os=$host_os" \
    --platforms="@gizos//tools/bazel/platforms:$platform" \
    //:embedded_menu_font >invalid-ttf-symbol.log 2>&1; then
    printf 'expected invalid embedded TTF symbol name to fail analysis\n' >&2
    exit 1
fi
grep -F "symbol must be a valid C identifier" invalid-ttf-symbol.log >/dev/null
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
    //:embedded_menu_font_test \
    //:font_consumer \
    //:font_consumer_test \
    //:font_native_component \
    //:i18n_runtime \
    //:native_component \
    //:package \
    //:private_bk_ap_task_policy_codegen \
    //:private_bk_cp_task_policy_codegen \
    //:private_esp_task_policy_codegen \
    //:private_bk_ap_task_policy_test \
    //:private_bk_cp_task_policy_test \
    //:private_esp_task_policy_test \
    //:runtime

python3 "$fixture_root/check_task_policy.py" "$consumer_root/bazel-bin" "$repository_root"
test ! -e "$consumer_root/private_esp_task_policy"
test ! -e "$consumer_root/private_bk_task_policy"

"${BAZEL_BIN:-bazel}" \
    --ignore_all_rc_files \
    --output_base="$consumer_root/output-base" \
    test \
    --enable_bzlmod \
    --noenable_workspace \
    --repository_cache="$repository_cache" \
    --override_module="gizos=$repository_root" \
    --define="h2_host_os=$host_os" \
    --platforms="@gizos//tools/bazel/platforms:$platform" \
    --extra_toolchains="@gizos//tools/bazel/platforms:${platform}_test_toolchain" \
    //:embedded_menu_font_test \
    //:font_consumer_test \
    //:private_esp_task_policy_test \
    //:private_bk_ap_task_policy_test \
    //:private_bk_cp_task_policy_test

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
