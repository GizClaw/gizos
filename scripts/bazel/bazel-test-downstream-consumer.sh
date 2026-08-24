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
    build \
    --enable_bzlmod \
    --noenable_workspace \
    --repository_cache="$repository_cache" \
    --override_module="gizos=$repository_root" \
    --define="h2_host_os=$host_os" \
    --platforms="@gizos//tools/bazel/platforms:$platform" \
    --extra_toolchains="@gizos//tools/bazel/platforms:${platform}_test_toolchain" \
    //:firmware_lib \
    //:native_component \
    //:package \
    //:runtime
