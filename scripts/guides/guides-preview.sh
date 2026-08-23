#!/bin/sh
set -eu

case "${GUIDES_PREVIEW_HOST-}" in ''|*[!A-Za-z0-9._:-]*) printf 'invalid GUIDES_PREVIEW_HOST\n' >&2; exit 2 ;; esac
case "${GUIDES_PREVIEW_PORT-}" in ''|*[!0-9]*) printf 'invalid GUIDES_PREVIEW_PORT\n' >&2; exit 2 ;; esac
test "$GUIDES_PREVIEW_PORT" -ge 1 && test "$GUIDES_PREVIEW_PORT" -le 65535 || {
  printf 'GUIDES_PREVIEW_PORT must be 1..65535\n' >&2
  exit 2
}
"$(dirname "$0")/guides-build.sh"
exec "${BAZEL_BIN:-bazel}" run //guides:preview -- --host "$GUIDES_PREVIEW_HOST" --port "$GUIDES_PREVIEW_PORT"
