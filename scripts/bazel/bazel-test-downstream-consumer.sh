#!/usr/bin/env bash

set -euo pipefail

repository_root=$(git rev-parse --show-toplevel)
fixture_root="$repository_root/tools/bazel/tests/downstream_consumer"
consumer_root=$(mktemp -d "${TMPDIR:-/tmp}/gizos-downstream-consumer.XXXXXX")
trap 'rm -rf "$consumer_root"' EXIT
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
cp "$fixture_root/layout.txt" "$consumer_root/layout.txt"
cp "$fixture_root/partition.csv" "$consumer_root/partition.csv"
cp "$fixture_root/ram_regions.csv" "$consumer_root/ram_regions.csv"
cp "$fixture_root/sdkconfig.h2loader.defaults" "$consumer_root/sdkconfig.h2loader.defaults"

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
    'set(//:private_bk_firmware //:private_esp_firmware)'

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
    //:bk3633_test_support_consumer \
    //:firmware_lib \
    //:i18n_runtime \
    //:native_component \
    //:package \
    //:runtime
