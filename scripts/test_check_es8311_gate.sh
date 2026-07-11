#!/usr/bin/env bash
# Copyright (c) 2026 Hsiu-Chi Tsai
# SPDX-License-Identifier: Apache-2.0
#
# Offline integration test for scripts/check_es8311_upstream_gate.sh.
# Puts a mock `gh` on PATH (no network, no auth) and asserts the VERDICT across
# the five scenarios the gate has to get right:
#   1. base merged + a live third-party ES8311 PR  -> ENGAGE (do not compete)
#   2. base merged + nobody driving ES8311         -> GO     (submit ours)
#   3. base NOT merged (only our board PR merged)  -> HOLD   (no in-tree consumer)
#   4. gh failing entirely                         -> HOLD   (never a false merge)
#   5. base merged but the *search* fails          -> HOLD   (never a false "nobody")
#
# 4 and 5 are the two fail-open traps, and they are mirror images: 4 would read a
# gh outage as "merged", 5 would read a rate-limited search as "nobody is driving
# it". Both must degrade to HOLD.
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
  [ "$SCEN" = "ghdown" ] && return 1
  local state="OPEN" merged="null"
  case "$SCEN:$pr" in
    # every scenario except "nobase" has the base board already merged
    engage:107655|go:107655|empty:107655|searchfail:107655)
                     state="MERGED"; merged="2026-06-11T21:14:01Z" ;;
    nobase:110205)   state="MERGED"; merged="2026-06-11T21:14:01Z" ;;
    *:107660)        state="CLOSED" ;;
  esac
  case "$f" in
    state) echo "$state" ;;
    mergedAt) echo "$merged" ;;
    reviewDecision) echo "APPROVED" ;;
    title) echo "fake title for $pr" ;;
  esac
}
search() {
  # The real `gh search` exits 0 with EMPTY output when there are genuinely no
  # matches, and non-zero when it could not run at all. The gate must tell those
  # two apart, so the mock reproduces both.
  [ "$SCEN" = "ghdown" ] && return 1
  [ "$SCEN" = "searchfail" ] && return 1
  # "empty" = no open es8311 PR at all, not even ours. This is the state we land in
  # once our board PR #110205 merges, so the empty-input path must be exercised.
  [ "$SCEN" = "empty" ] && return 0
  printf '110205\tthc1006\t2026-07-11\tM5Stack StickS3\n'
  [ "$SCEN" = "engage" ] && printf '109999\tsomebody\t2026-07-10\tES8311 codec driver, take 2\n'
  return 0
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
    printf 'PASS  %-11s %s\n' "$1" "$3"; pass=$((pass+1))
  else
    printf 'FAIL  %-11s %s\n      got: %s\n' "$1" "$3" "$verdict"; fail=$((fail+1))
  fi
}

check engage     'VERDICT: ENGAGE' 'base merged + live 3rd-party PR -> engage'
check go         'VERDICT: GO'     'base merged + nobody on it     -> submit ours'
check empty      'VERDICT: GO'     'search returns nothing at all  -> submit ours'
check nobase     'VERDICT: HOLD'   'base not merged                -> no consumer'
check ghdown     'VERDICT: HOLD'   'gh down                        -> no false merge'
check searchfail 'VERDICT: HOLD'   'search failed                  -> no false nobody'

echo "----------------------------------------"
echo "PASS=$pass FAIL=$fail"
[ "$fail" -eq 0 ]
