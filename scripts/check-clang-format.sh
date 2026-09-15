#!/usr/bin/env bash
set -euo pipefail

# Checks C/C++ formatting with a pinned clang-format.
#   (default)     check lines changed since the base revision
#   --fix         format lines changed since the base revision
#   --all         check every in-scope file
#   --all --fix   format every in-scope file
# The base revision is $CLANG_FORMAT_BASE, or the merge-base with origin/main.

readonly required_version=22.1.5
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
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/clang-format.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT

# clang-format and git-clang-format come from the same install.
if [[ -n "${CLANG_FORMAT:-}" ]]; then
  formatter=$CLANG_FORMAT
  git_formatter=(git-clang-format)
elif command -v clang-format >/dev/null 2>&1 &&
  [[ "$(clang-format --version 2>/dev/null)" == *"version $required_version"* ]]; then
  formatter=$(command -v clang-format)
  git_formatter=(git-clang-format)
elif command -v uv >/dev/null 2>&1; then
  formatter="$tmp_dir/clang-format"
  printf '#!/bin/sh\nexec uv tool run --from clang-format==%s clang-format "$@"\n' "$required_version" >"$formatter"
  chmod +x "$formatter"
  git_formatter=(uv tool run --from "clang-format==$required_version" git-clang-format)
else
  echo "clang-format $required_version is required." >&2
  echo "Install it (pipx install clang-format==$required_version), install uv, or set CLANG_FORMAT." >&2
  exit 1
fi

extensions=(c h cc cpp hpp cxx hh m mm)

if [[ "$scope" == changed ]]; then
  base=${CLANG_FORMAT_BASE:-$(git merge-base HEAD origin/main)}
  ext_list=$(IFS=,; echo "${extensions[*]}")
  if [[ "$mode" == fix ]]; then
    "${git_formatter[@]}" --binary "$formatter" --extensions "$ext_list" --force "$base"
    exit 0
  fi
  output=$("${git_formatter[@]}" --binary "$formatter" --extensions "$ext_list" --diff "$base" || true)
  if [[ -z "$output" || "$output" == *"no modified files to format"* ||
    "$output" == *"did not modify any files"* ]]; then
    exit 0
  fi
  printf '%s\n\n' "$output"
  echo "clang-format wants the changes above in lines changed since $base." >&2
  echo "Run 'make format' or 'scripts/check-clang-format.sh --fix' to apply them." >&2
  exit 1
fi

file_list="$tmp_dir/files"
git ls-files -z | while IFS= read -r -d '' file; do
  case "${file##*.}" in
    c | h | cc | cpp | hpp | cxx | hh | m | mm) printf '%s\0' "$file" ;;
  esac
done >"$file_list"
[[ -s "$file_list" ]] || exit 0

jobs=${CLANG_FORMAT_JOBS:-4}
if [[ "$mode" == fix ]]; then
  xargs -0 -n 64 -P "$jobs" "$formatter" -i <"$file_list"
elif ! xargs -0 -n 64 -P "$jobs" "$formatter" --dry-run --Werror <"$file_list"; then
  echo >&2
  echo "clang-format found files that need formatting (listed above)." >&2
  echo "Run 'scripts/check-clang-format.sh --all --fix' to fix them." >&2
  exit 1
fi
