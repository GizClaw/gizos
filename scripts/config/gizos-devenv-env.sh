#!/bin/sh
# Sourced by h2loader-operation-env.sh with REPO_ROOT set. Never installs tools.
if command -v "${BUN_BIN:-bun}" >/dev/null 2>&1; then
  _gizos_devenv_exports=$("${BUN_BIN:-bun}" "$REPO_ROOT/tools/gizos/src/cli.ts" env --shell sh) || return 1
  eval "$_gizos_devenv_exports" || return 1
  unset _gizos_devenv_exports
fi

# Existing operator installations remain usable before adoption into the store.
if [ -z "${IDF_PATH-}" ] && [ -z "${BK7258_PATH-}" ] && [ -f "$REPO_ROOT/.env/devenv" ]; then
  . "$REPO_ROOT/.env/devenv" || return 1
fi
