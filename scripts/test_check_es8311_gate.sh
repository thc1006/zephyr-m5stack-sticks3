#!/usr/bin/env bash
# Copyright (c) 2026 Hsiu-Chi Tsai
# SPDX-License-Identifier: Apache-2.0
#
# Offline integration test for scripts/check_es8311_upstream_gate.sh.
# Puts a mock `gh` on PATH (no network, no auth) and asserts the gate VERDICT
# across the four scenarios that the maintainer review on PR #16 called out:
#   1. base #107655 MERGED + a live successor PR        -> GATE OPEN
#   2. base #107655 MERGED but NO successor             -> STILL HOLDING (AND, not OR)
#   3. only our own board #110205 MERGED (old bug)      -> STILL HOLDING
#   4. gh failing entirely (old "?" false-merge bug)    -> STILL HOLDING (never false-open)
#
# Usage: bash scripts/test_check_es8311_gate.sh
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
gate="$here/check_es8311_upstream_gate.sh"
td=$(mktemp -d)
trap 'rm -rf "$td"' EXIT
mkdir -p "$td/bin"

cat > "$td/bin/gh" <<'MOCK'
#!/usr/bin/env bash
# Fake gh; $SCEN selects the scenario. Mirrors the shape the real gh returns.
get_json_field() { local p=""; while [ $# -gt 0 ]; do [ "$1" = "--json" ] && p="$2"; shift; done; echo "$p"; }
prview() {
  local pr="$1"; shift; local f; f=$(get_json_field "$@")
  [ "$SCEN" = "fail" ] && return 1
  local state="OPEN" merged="null"
  case "$SCEN:$pr" in
    open_succ:107655|base_only:107655) state="MERGED"; merged="2026-06-09T10:00:00Z" ;;
    ourboard:110205)                    state="MERGED"; merged="2026-06-09T10:00:00Z" ;;
  esac
  case "$f" in
    state) echo "$state" ;;
    mergedAt) echo "$merged" ;;
    reviewDecision) echo "APPROVED" ;;
    title) echo "fake title for $pr" ;;
  esac
}
search() {
  [ "$SCEN" = "fail" ] && return 1
  printf '107660\tnnSiD\t2026-06-05\tES8311 codec driver\n'
  printf '110205\tthc1006\t2026-06-03\tM5Stack StickS3\n'
  [ "$SCEN" = "open_succ" ] && printf '109999\tnnSiD\t2026-06-10\tES8311 v2 split codec PR\n'
}
case "$1 $2" in
  "pr view") shift 2; prview "$@" ;;
  "search prs") search ;;
  *) exit 0 ;;
esac
MOCK
chmod +x "$td/bin/gh"

pass=0 fail=0
check() { # SCEN EXPECT_REGEX label
  local out verdict
  out=$(SCEN="$1" PATH="$td/bin:$PATH" bash "$gate")
  verdict=$(echo "$out" | grep VERDICT)
  if echo "$verdict" | grep -qE "$2"; then
    printf 'PASS  %-12s %s\n' "$1" "$3"; pass=$((pass+1))
  else
    printf 'FAIL  %-12s %s\n      got: %s\n' "$1" "$3" "$verdict"; fail=$((fail+1))
  fi
}

check open_succ 'GATE OPEN'     'base merged + live successor -> open'
check base_only 'STILL HOLDING' 'base merged, no successor -> hold (AND)'
check ourboard  'STILL HOLDING' 'only our #110205 merged -> hold (old false-open)'
check fail      'STILL HOLDING' 'gh down -> hold (old "?" false-merge)'

echo "----------------------------------------"
echo "PASS=$pass FAIL=$fail"
[ "$fail" -eq 0 ]
