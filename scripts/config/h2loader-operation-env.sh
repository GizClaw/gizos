#!/bin/sh

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)

h2_color_enabled() {
  [ "${NO_COLOR+x}" = x ] && return 1

  case "${FORCE_COLOR:-}" in
    ''|0|false|FALSE|no|NO) ;;
    *) return 0 ;;
  esac

  case "${H2LOADER_COLOR:-auto}" in
    always|1|true|TRUE|yes|YES) return 0 ;;
    never|0|false|FALSE|no|NO) return 1 ;;
  esac

  [ -z "${CI:-}" ] && [ "${TERM:-}" != dumb ] && [ -t 1 ]
}

h2_log() {
  _h2_log_level=$1
  shift
  case "$_h2_log_level" in
    OK) _h2_log_color=32 ;;
    WARNING) _h2_log_color=33 ;;
    ERROR) _h2_log_color=31 ;;
    *) _h2_log_color=36 ;;
  esac

  if h2_color_enabled; then
    printf '\033[%sm[%s]\033[0m %s\n' "$_h2_log_color" "$_h2_log_level" "$*"
  else
    printf '[%s] %s\n' "$_h2_log_level" "$*"
  fi
}

load_env_file() {
  unset _h2_env_label _h2_env_path _h2_env_snapshot _h2_env_exports _h2_env_name
  _h2_env_label=$1
  _h2_env_path=$2
  if [ ! -f "$_h2_env_path" ]; then
    h2_log WARNING "env missing: $_h2_env_label"
    return 0
  fi

  if _h2_env_snapshot=$(
    set -a
    . "$_h2_env_path" >/dev/null 2>&1 || exit 1
    set +a
    export -p
  ); then
    _h2_env_exports=$(export -p | /usr/bin/sed -n 's/^export \([A-Za-z_][A-Za-z0-9_]*\).*$/\1/p')
    for _h2_env_name in $_h2_env_exports; do
      unset "$_h2_env_name"
    done
    eval "$_h2_env_snapshot"
    h2_log OK "env loaded: $_h2_env_label"
  else
    h2_log ERROR "env invalid: $_h2_env_label"
    return 1
  fi
}

load_env_file '.env/devenv' "$REPO_ROOT/.env/devenv" || exit 1
if [ -n "${HOME:-}" ]; then
  load_env_file '~/.config/h2loader/env' "$HOME/.config/h2loader/env" || :
else
  h2_log WARNING 'env missing: ~/.config/h2loader/env'
fi

if h2_color_enabled; then
  H2LOADER_COLOR=always
else
  H2LOADER_COLOR=never
fi
export H2LOADER_COLOR

exec "$@"
