#!/bin/sh
# Sourced by h2loader-operation-env.sh with REPO_ROOT set.

_gizos_devenv_explicit=${GIZOS_DEVENV_ROOT:+yes}
_gizos_devenv_candidate=${GIZOS_DEVENV_ROOT:-$REPO_ROOT/../gizos-devenv}
if [ ! -d "$_gizos_devenv_candidate" ]; then
  if [ "$_gizos_devenv_explicit" = yes ]; then
    printf '%s\n' 'Configured GIZOS_DEVENV_ROOT is missing; run make devenv' >&2
    return 1
  fi
  # Existing private setups remain usable during migration. Public checkouts
  # have no legacy adapter and can still run host-only targets without an SDK.
  if [ -f "$REPO_ROOT/.env/devenv" ]; then
    . "$REPO_ROOT/.env/devenv" || return 1
  fi
  unset _gizos_devenv_explicit _gizos_devenv_candidate
  return 0
fi

GIZOS_DEVENV_ROOT=$(CDPATH= cd -- "$_gizos_devenv_candidate" && pwd -P) || return 1
_gizos_devenv_expected=$(cat "$REPO_ROOT/tools/bazel/native_versions/gizos_devenv_commit.txt") || return 1
_gizos_devenv_actual=$(git -C "$GIZOS_DEVENV_ROOT" rev-parse HEAD) || return 1
if [ "$_gizos_devenv_actual" != "$_gizos_devenv_expected" ]; then
  printf '%s\n' 'gizos-devenv revision does not match the GizOS pin; use the pinned checkout' >&2
  return 1
fi
_gizos_devenv_dirty=$(git -C "$GIZOS_DEVENV_ROOT" status --porcelain=v1 --untracked-files=no) || return 1
if [ -n "$_gizos_devenv_dirty" ]; then
  printf '%s\n' 'gizos-devenv has tracked changes; use an unchanged pinned checkout' >&2
  return 1
fi
GIZOS_ROOT=$REPO_ROOT
export GIZOS_DEVENV_ROOT GIZOS_ROOT
. "$GIZOS_DEVENV_ROOT/export.sh" || return 1
unset _gizos_devenv_explicit _gizos_devenv_candidate
unset _gizos_devenv_expected _gizos_devenv_actual _gizos_devenv_dirty
