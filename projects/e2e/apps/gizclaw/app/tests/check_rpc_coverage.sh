#!/bin/sh
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$script_dir/../../../../../.." && pwd)
source_dir="$script_dir/../src"
manifest="$script_dir/rpc_coverage.tsv"
valid_fixture="$script_dir/rpc_evidence_valid.tsv"
malformed_fixture="$script_dir/rpc_evidence_malformed.tsv"
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/h2-gizclaw-coverage.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

validate_manifest() {
  awk -F '\t' '
  /^#/ { next }
  NF != 5 { print "invalid manifest row: " $0 > "/dev/stderr"; bad = 1; next }
  $1 != "method" && $1 != "wrapper" {
    print "invalid kind: " $1 > "/dev/stderr"; bad = 1
  }
  $3 != "live" && $3 != "profile-gated" && $3 != "primitive" &&
      $3 != "integration" && $3 != "sdk-gap" && $3 != "Edge-only" &&
      $3 != "cleanup" {
    print "invalid mode: " $3 > "/dev/stderr"; bad = 1
  }
  seen[$1 SUBSEP $2]++ { print "duplicate: " $1 " " $2 > "/dev/stderr"; bad = 1 }
  ($3 == "live" || $3 == "cleanup") && $5 == "-" {
    print "missing evidence mapping: " $1 " " $2 > "/dev/stderr"; bad = 1
  }
  $1 == "wrapper" && ($3 == "live" || $3 == "cleanup") && $5 != $2 {
    print "wrapper evidence must be its own symbol: " $2 > "/dev/stderr"; bad = 1
  }
  {
    kind[$2] = $1
    mode[$2] = $3
    evidence[$2] = $5
  }
  END {
    for (symbol in evidence) {
      mapped = evidence[symbol]
      if (kind[symbol] != "method" ||
          (mode[symbol] != "live" && mode[symbol] != "cleanup") ||
          mapped == symbol) {
        continue
      }
      if (kind[mapped] != "wrapper" ||
          (mode[mapped] != "live" && mode[mapped] != "cleanup")) {
        print "method evidence is not a live wrapper: " symbol " -> " mapped \
          > "/dev/stderr"
        bad = 1
      }
    }
    exit bad
  }
' "$1"
}

validate_manifest "$manifest"
validate_manifest "$valid_fixture"
if validate_manifest "$malformed_fixture" >/dev/null 2>&1; then
  echo "malformed evidence fixture was accepted" >&2
  exit 1
fi
cp "$valid_fixture" "$tmp_dir/duplicate.tsv"
tail -n 1 "$valid_fixture" >> "$tmp_dir/duplicate.tsv"
if validate_manifest "$tmp_dir/duplicate.tsv" >/dev/null 2>&1; then
  echo "duplicate evidence fixture was accepted" >&2
  exit 1
fi

grep -Eoh \
  'H2_GIZCLAW_RPC_(ALL|CLIENT|SERVER|RUNTIME)_[A-Z0-9_]+' \
  "$repo_root/libs/gizclaw/include/h2_gizclaw_rpc.h" | sort -u \
  > "$tmp_dir/current-methods"
awk -F '\t' '$1 == "method" { print $2 }' "$manifest" | sort -u \
  > "$tmp_dir/manifest-methods"

perl -0777 -ne '
  while (/\b(?:int|void|bool|h2_pal_result_t)\s+(h2_gizclaw_(?:client|conversation|rpc|speech)_[a-z0-9_]+)\s*\(/g) {
    print "$1\n";
  }
' "$repo_root"/libs/gizclaw/include/*.h | sort -u \
  > "$tmp_dir/current-wrappers"
awk -F '\t' '$1 == "wrapper" { print $2 }' "$manifest" | sort -u \
  > "$tmp_dir/manifest-wrappers"

if ! diff -u "$tmp_dir/current-methods" "$tmp_dir/manifest-methods"; then
  echo "RPC method coverage manifest is stale" >&2
  exit 1
fi
if ! diff -u "$tmp_dir/current-wrappers" "$tmp_dir/manifest-wrappers"; then
  echo "public wrapper coverage manifest is stale" >&2
  exit 1
fi

tab=$(printf '\t')
while IFS="$tab" read -r kind symbol mode stage evidence; do
  case "$kind" in
    \#*|'') continue ;;
  esac
  case "$mode" in
    live|cleanup)
      if ! perl -0777 -e \
        'exit((join "", <>) =~ /(?:checked|h2_gizclaw_e2e_evidence)\s*\(\s*"'"$evidence"'"/ ? 0 : 1)' \
        "$source_dir/h2_gizclaw_e2e_fixture.c" \
        "$source_dir/h2_gizclaw_e2e_concurrency.c" \
        "$source_dir/h2_gizclaw_e2e_firmware.c" \
        "$source_dir/h2_gizclaw_e2e_rpc.c" \
        "$source_dir/h2_gizclaw_e2e_voice.c"; then
        echo "missing E2E evidence emission: $symbol -> $evidence ($stage)" >&2
        exit 1
      fi
      ;;
  esac
done < "$manifest"

evidence_count=$(awk -F '\t' \
  '$1 == "method" && ($3 == "live" || $3 == "cleanup") { count++ } END { print count + 0 }' \
  "$manifest")
echo "H2_GIZCLAW_E2E coverage=PASS methods=$(wc -l < "$tmp_dir/current-methods" | tr -d ' ') wrappers=$(wc -l < "$tmp_dir/current-wrappers" | tr -d ' ') evidence=$evidence_count"
