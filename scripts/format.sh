#!/usr/bin/env bash
set -euo pipefail

# Formats C/C++ (clang-format), Lua (StyLua) and Bazel files (buildifier)
# with pinned tool versions.
#   (default)     check changes since the base revision
#   --fix         format changes since the base revision
#   --all         check every in-scope file
#   --all --fix   format every in-scope file
# The base revision is $FORMAT_BASE, or the merge-base with origin/main.
# clang-format only touches changed lines; StyLua and buildifier format whole
# changed files. Tool binaries can be overridden with $CLANG_FORMAT, $STYLUA
# and $BUILDIFIER.

readonly clang_format_version=22.1.5
readonly stylua_version=2.5.2
readonly buildifier_version=10.0.1

mode=check
scope=changed
for arg in "$@"; do
  case "$arg" in
    --fix) mode=fix ;;
    --all) scope=all ;;
    *)
      echo "usage: $0 [--all] [--fix]" >&2
      exit 2
      ;;
  esac
done

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/format.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT
cache_dir="${XDG_CACHE_HOME:-$HOME/.cache}/h2-format-tools"

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d' ' -f1
  else
    shasum -a 256 "$1" | cut -d' ' -f1
  fi
}

platform() {
  local os arch
  case "$(uname -s)" in
    Darwin) os=darwin ;;
    Linux) os=linux ;;
    *) echo "unsupported OS: $(uname -s)" >&2; return 1 ;;
  esac
  case "$(uname -m)" in
    arm64 | aarch64) arch=arm64 ;;
    x86_64 | amd64) arch=amd64 ;;
    *) echo "unsupported CPU: $(uname -m)" >&2; return 1 ;;
  esac
  echo "$os-$arch"
}

# download_tool <name> <version> <url> <sha256> <archive: zip|raw>
download_tool() {
  local name=$1 version=$2 url=$3 sha=$4 kind=$5
  local dir="$cache_dir/$name-$version"
  if [[ ! -x "$dir/$name" ]]; then
    echo "Downloading $name $version" >&2
    local file="$tmp_dir/$name.download"
    curl -fsSL --retry 3 -o "$file" "$url"
    if [[ "$(sha256_of "$file")" != "$sha" ]]; then
      echo "$name download checksum mismatch: $url" >&2
      return 1
    fi
    mkdir -p "$tmp_dir/$name.dir"
    if [[ "$kind" == zip ]]; then
      unzip -q -o "$file" -d "$tmp_dir/$name.dir"
    else
      cp "$file" "$tmp_dir/$name.dir/$name"
    fi
    chmod +x "$tmp_dir/$name.dir/$name"
    mkdir -p "$cache_dir"
    rm -rf "$dir"
    mv "$tmp_dir/$name.dir" "$dir"
  fi
  echo "$dir/$name"
}

resolve_stylua() {
  if [[ -n "${STYLUA:-}" ]]; then echo "$STYLUA"; return; fi
  if command -v stylua >/dev/null 2>&1 &&
    [[ "$(stylua --version 2>/dev/null)" == "stylua $stylua_version" ]]; then
    command -v stylua
    return
  fi
  local asset sha
  case "$(platform)" in
    darwin-arm64) asset=macos-aarch64 sha=92ff0889e16324801bc072692974bb67f8161e62010fc90f96c62a17f81f32c7 ;;
    darwin-amd64) asset=macos-x86_64 sha=53c50a1605d0a6345d160a1a5a21db40bcf2bf9cd23c17f7c277a63a1bff3a7f ;;
    linux-arm64) asset=linux-aarch64 sha=0ef2ebf0b7e5a652b65c4cb96c6d9ffb3981a98547de3c764465bbf54a8d761a ;;
    linux-amd64) asset=linux-x86_64 sha=bcb0d855e91f102f28a370e850f8566b3b44b79e6274d806ea5246837c0fd5ab ;;
  esac
  download_tool stylua "$stylua_version" \
    "https://github.com/JohnnyMorganz/StyLua/releases/download/v$stylua_version/stylua-$asset.zip" "$sha" zip
}

resolve_buildifier() {
  if [[ -n "${BUILDIFIER:-}" ]]; then echo "$BUILDIFIER"; return; fi
  if command -v buildifier >/dev/null 2>&1 &&
    [[ "$(buildifier --version 2>/dev/null)" == *"version: $buildifier_version"* ]]; then
    command -v buildifier
    return
  fi
  local sha
  case "$(platform)" in
    darwin-arm64) sha=afb78f350319b59cc51d6add3a5f3ba68e63e5d88f68c5a9ea6328a07084d319 ;;
    darwin-amd64) sha=1d02bb9148cadf2cbee330f9bd657352c765b52b68a03d970e10e47706bdc436 ;;
    linux-arm64) sha=6d7aebd23aa85847a66d517bb6220d95f24a2752e62cce0f089145b680b539c7 ;;
    linux-amd64) sha=e0ea28e2d639347724435ebafe0531fd764fbf20eec6a23000c81edd0d58e51d ;;
  esac
  download_tool buildifier "$buildifier_version" \
    "https://github.com/bazelbuild/buildtools/releases/download/v$buildifier_version/buildifier-$(platform)" "$sha" raw
}

# clang-format and git-clang-format come from the same install.
resolve_clang_format() {
  if [[ -n "${CLANG_FORMAT:-}" ]]; then
    clang_format=$CLANG_FORMAT
    git_clang_format=(git-clang-format)
  elif command -v clang-format >/dev/null 2>&1 &&
    [[ "$(clang-format --version 2>/dev/null)" == *"version $clang_format_version"* ]]; then
    clang_format=$(command -v clang-format)
    git_clang_format=(git-clang-format)
  elif command -v uv >/dev/null 2>&1; then
    clang_format="$tmp_dir/clang-format"
    printf '#!/bin/sh\nexec uv tool run --from clang-format==%s clang-format "$@"\n' "$clang_format_version" >"$clang_format"
    chmod +x "$clang_format"
    git_clang_format=(uv tool run --from "clang-format==$clang_format_version" git-clang-format)
  else
    echo "clang-format $clang_format_version is required." >&2
    echo "Install it (pipx install clang-format==$clang_format_version), install uv, or set CLANG_FORMAT." >&2
    return 1
  fi
}

is_c_file() {
  case "$1" in
    *.c | *.h | *.cc | *.cpp | *.hpp | *.cxx | *.hh | *.m | *.mm) return 0 ;;
  esac
  return 1
}

is_lua_file() {
  [[ "$1" == *.lua ]]
}

is_bazel_file() {
  case "${1##*/}" in
    BUILD | BUILD.bazel | MODULE.bazel | WORKSPACE | WORKSPACE.bazel | *.bzl | *.BUILD.bazel | *.BUILD) return 0 ;;
  esac
  return 1
}

if [[ "$scope" == changed ]]; then
  base=${FORMAT_BASE:-$(git merge-base HEAD origin/main)}
  git diff -z --name-only --diff-filter=ACMR "$base" >"$tmp_dir/candidates"
else
  git ls-files -z >"$tmp_dir/candidates"
fi

: >"$tmp_dir/c"
: >"$tmp_dir/lua"
: >"$tmp_dir/bazel"
while IFS= read -r -d '' file; do
  [[ -f "$file" ]] || continue
  if is_c_file "$file"; then
    printf '%s\0' "$file" >>"$tmp_dir/c"
  elif is_lua_file "$file"; then
    printf '%s\0' "$file" >>"$tmp_dir/lua"
  elif is_bazel_file "$file"; then
    printf '%s\0' "$file" >>"$tmp_dir/bazel"
  fi
done <"$tmp_dir/candidates"

jobs=${FORMAT_JOBS:-4}
failed=()

if [[ -s "$tmp_dir/c" ]]; then
  resolve_clang_format
  if [[ "$scope" == changed ]]; then
    ext_list=c,h,cc,cpp,hpp,cxx,hh,m,mm
    if [[ "$mode" == fix ]]; then
      # git-clang-format exits non-zero whenever it rewrote files.
      "${git_clang_format[@]}" --binary "$clang_format" --extensions "$ext_list" --force "$base" >/dev/null || true
    else
      output=$("${git_clang_format[@]}" --binary "$clang_format" --extensions "$ext_list" --diff "$base" || true)
      if [[ -n "$output" && "$output" != *"no modified files to format"* &&
        "$output" != *"did not modify any files"* ]]; then
        printf '%s\n\n' "$output"
        failed+=(clang-format)
      fi
    fi
  elif [[ "$mode" == fix ]]; then
    xargs -0 -n 64 -P "$jobs" "$clang_format" -i <"$tmp_dir/c" || failed+=(clang-format)
  elif ! xargs -0 -n 64 -P "$jobs" "$clang_format" --dry-run --Werror <"$tmp_dir/c"; then
    failed+=(clang-format)
  fi
fi

if [[ -s "$tmp_dir/lua" ]]; then
  stylua=$(resolve_stylua)
  if [[ "$mode" == fix ]]; then
    # StyLua 2.5.2 can need several passes to reach a stable result.
    for _ in 1 2 3 4 5; do
      if ! xargs -0 "$stylua" --respect-ignores <"$tmp_dir/lua"; then
        failed+=(stylua)
        break
      fi
      xargs -0 "$stylua" --respect-ignores --check <"$tmp_dir/lua" >/dev/null 2>&1 && break
    done
  elif ! xargs -0 "$stylua" --respect-ignores --check <"$tmp_dir/lua"; then
    failed+=(stylua)
  fi
fi

if [[ -s "$tmp_dir/bazel" ]]; then
  buildifier=$(resolve_buildifier)
  if [[ "$mode" == fix ]]; then
    xargs -0 "$buildifier" --mode=fix --lint=off <"$tmp_dir/bazel" || failed+=(buildifier)
  elif ! xargs -0 "$buildifier" --mode=check --lint=off <"$tmp_dir/bazel"; then
    failed+=(buildifier)
  fi
fi

if [[ "${#failed[@]}" -gt 0 ]]; then
  echo >&2
  if [[ "$mode" == fix ]]; then
    echo "Formatting failed (${failed[*]}); see the errors above." >&2
  else
    echo "Formatting needed (${failed[*]}). Run 'make format' to apply it." >&2
  fi
  exit 1
fi
