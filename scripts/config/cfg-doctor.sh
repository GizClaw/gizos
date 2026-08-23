#!/bin/sh

case "$(uname -s):$(uname -m)" in
  Darwin:arm64) host_config=macos_arm64 ;;
  Linux:x86_64) host_config=linux_x86_64 ;;
  Linux:aarch64|Linux:arm64) host_config=linux_arm64 ;;
  *)
    printf '[ERROR] unsupported native H2Loader CLI host: %s\n' "$(uname -s):$(uname -m)" >&2
    exit 2
    ;;
esac

exec scripts/config/h2loader-operation-env.sh \
  "${BAZEL_BIN:-bazel}" run "--config=$host_config" \
  //projects/h2loader/targets/cc_binary/cli:h2loader -- check
