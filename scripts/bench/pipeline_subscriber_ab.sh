#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

ANALYZER="${1:-./build/stack_usage_analyzer}"
if [[ $# -gt 0 ]]; then
  shift
fi

if [[ $# -gt 0 ]]; then
  FILES=("$@")
else
  FILES=(
    "test/alloca/oversized-constant.c"
    "test/resource-lifetime/local-double-release.c"
    "test/uninitialized-variable/uninitialized-local-unused.c"
    "test/diagnostics/duplicate-else-if-basic.c"
    "test/integer-overflow/cross-tu-tricky-use.c"
  )
fi

for file in "${FILES[@]}"; do
  if [[ ! -f "$file" ]]; then
    echo "missing input file: $file" >&2
    exit 1
  fi
done

run_case() {
  local label="$1"
  local subscriber_flag="$2"

  local tmp
  tmp="$(mktemp "/tmp/ct_pipeline_ab_${label}.XXXXXX")"
  (
    for file in "${FILES[@]}"; do
      if [[ "$subscriber_flag" == "1" ]]; then
        CTRACE_PIPELINE_SUBSCRIBERS=1 "$ANALYZER" "$file" --warnings-only >/dev/null 2>&1
      else
        "$ANALYZER" "$file" --warnings-only >/dev/null 2>&1
      fi
    done
  ) > /dev/null 2>&1

  /usr/bin/time -p bash -c '
    set -euo pipefail
    for file in "$@"; do
      if [[ "'"$subscriber_flag"'" == "1" ]]; then
        CTRACE_PIPELINE_SUBSCRIBERS=1 "'"$ANALYZER"'" "$file" --warnings-only >/dev/null 2>&1
      else
        "'"$ANALYZER"'" "$file" --warnings-only >/dev/null 2>&1
      fi
    done
  ' _ "${FILES[@]}" 2>"$tmp"

  local real user sys
  real="$(awk "/^real /{print \$2}" "$tmp")"
  user="$(awk "/^user /{print \$2}" "$tmp")"
  sys="$(awk "/^sys /{print \$2}" "$tmp")"
  rm -f "$tmp"

  echo "$label: real=${real}s user=${user}s sys=${sys}s"
}

echo "Analyzer: $ANALYZER"
echo "Inputs (${#FILES[@]}): ${FILES[*]}"
run_case "baseline" "0"
run_case "subscriber" "1"
