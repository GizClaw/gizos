#!/bin/sh
# Build the native h2loader CLI for this host and print its absolute path on
# stdout (everything else goes to stderr), so it can be used inline:
#
#   $(make h2loader-bin) --port <serial-port> send --file bazel-bin/.../x.update.tar.zlib
#
# Running the binary from the caller's shell keeps the caller's cwd, so
# relative package paths and bazel-bin symlinks resolve as typed.
set -eu

bazel=${BAZEL_BIN:-bazel}
config=${H2_BAZEL_CONFIG:-${BAZEL_CONFIG:-}}
if [ -z "$config" ]; then
  case "$(uname -s)" in
    Darwin) config=macos ;;
    Linux) config=linux ;;
    *) echo "h2loader-bin: unsupported host $(uname -s); set BAZEL_CONFIG" >&2; exit 1 ;;
  esac
fi

target=//projects/h2loader/targets/cc_binary/cli:h2loader
"$bazel" build --config="$config" "$target" >&2
bin_dir=$("$bazel" info --config="$config" bazel-bin 2>/dev/null)
printf '%s/projects/h2loader/targets/cc_binary/cli/h2loader\n' "$bin_dir"
