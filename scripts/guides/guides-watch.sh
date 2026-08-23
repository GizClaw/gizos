#!/bin/sh
set -eu

case "${GUIDES_WATCH_HOST-}" in ''|*[!A-Za-z0-9._:-]*) printf 'invalid GUIDES_WATCH_HOST\n' >&2; exit 2 ;; esac
case "${GUIDES_WATCH_PORT-}" in ''|*[!0-9]*) printf 'invalid GUIDES_WATCH_PORT\n' >&2; exit 2 ;; esac
test "$GUIDES_WATCH_PORT" -ge 1 && test "$GUIDES_WATCH_PORT" -le 65535 || {
  printf 'GUIDES_WATCH_PORT must be 1..65535\n' >&2
  exit 2
}
exec "${BAZEL_BIN:-bazel}" run //guides:dev -- --host "$GUIDES_WATCH_HOST" --port "$GUIDES_WATCH_PORT"
