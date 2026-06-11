#!/usr/bin/env bash
# Copyright (c) 2026 Hsiu-Chi Tsai
# SPDX-License-Identifier: Apache-2.0
#
# Issue #7 monitor: report whether the gate to upstream the ES8311 codec driver
# has opened. Per ADR 0004 the gate is BOTH of, not either of:
#   (1) the base board PR #107655 has MERGED, AND
#   (2) the ES8311 work has resumed on a *live successor* PR — an open es8311
#       PR that is neither the stalled original (#107660) nor one of ours.
# Only when both hold do we print GATE OPEN. Our own board PR (#110205) merging
# on its own is NOT the gate (that was the old, too-loose trigger). Needs `gh`
# authenticated.
#
# Usage: bash scripts/check_es8311_upstream_gate.sh
set -euo pipefail

Z=zephyrproject-rtos/zephyr
BASE_PR=107655                       # ADR 0004 gate (1): base board lands first
STALLED_PR=107660                    # known-stalled original ES8311 PR; not a successor
OUR_PRS=(110205)                     # our own board PR(s) — never a successor
OUR_AUTHORS=(thc1006 junnncct1106)   # our authors — their es8311 PRs are not a successor

# field PR JSON_FIELD -> value, or "" on ANY gh error.
# Returns empty (not "?") on failure so a gh outage / auth gap / rate-limit can
# never be mistaken for a real value such as a false "merged" timestamp.
field() { gh pr view "$1" --repo "$Z" --json "$2" -q ".$2" 2>/dev/null || true; }

# is_merged PR -> success only if the PR is *actually* merged.
# Authoritative signal is state==MERGED (verified against the live GitHub API);
# mergedAt must also be a real ISO-8601 timestamp. "", "null" and "?" all fail.
is_merged() {
	local state merged
	state=$(field "$1" state)
	merged=$(field "$1" mergedAt)
	[ "$state" = "MERGED" ] && [[ "$merged" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}T ]]
}

# in_list NEEDLE ITEM... -> success if NEEDLE equals one of ITEM.
in_list() {
	local needle="$1"; shift
	local x
	for x in "$@"; do [ "$x" = "$needle" ] && return 0; done
	return 1
}

echo "ES8311 upstream gate — $(date -u '+%Y-%m-%d %H:%MZ')"
echo "----------------------------------------------------"

for pr in "$BASE_PR" "$STALLED_PR" "${OUR_PRS[@]}"; do
	state=$(field "$pr" state)
	merged=$(field "$pr" mergedAt)
	review=$(field "$pr" reviewDecision)
	title=$(field "$pr" title)
	printf '#%-7s %-6s merged=%-21s review=%-18s %s\n' \
		"$pr" "${state:-?}" "${merged:-no}" "${review:-none}" "${title:0:48}"
done

# Gate (1): base board must be MERGED.
base_merged=0
if is_merged "$BASE_PR"; then base_merged=1; fi

# Gate (2): a live successor — any OPEN es8311 PR that is neither the stalled
# original nor one of ours. Reuses the upstream search; "*" marks a candidate.
echo "--- live ES8311 PRs upstream ---"
successor=0
while IFS=$'\t' read -r num author updated title; do
	[ -z "$num" ] && continue
	mark=' '
	if ! in_list "$num" "$STALLED_PR" "${OUR_PRS[@]}" \
		&& ! in_list "$author" "${OUR_AUTHORS[@]}"; then
		mark='*'; successor=1
	fi
	printf ' %s #%-7s %-14s %s — %s\n' "$mark" "$num" "$author" "$updated" "${title:0:50}"
done < <(gh search prs --repo "$Z" es8311 --state open \
	--json number,author,updatedAt,title \
	-q '.[] | "\(.number)\t\(.author.login)\t\(.updatedAt[0:10])\t\(.title[0:50])"' 2>/dev/null || true)
[ "$successor" = 1 ] && echo " (* = candidate live successor PR)"

echo "----------------------------------------------------"
printf 'gate(1) base #%s merged   : %s\n' "$BASE_PR" "$([ "$base_merged" = 1 ] && echo yes || echo no)"
printf 'gate(2) live successor PR : %s\n' "$([ "$successor" = 1 ] && echo yes || echo no)"
if [ "$base_merged" = 1 ] && [ "$successor" = 1 ]; then
	echo "VERDICT: GATE OPEN — base board landed AND ES8311 resumed on a live PR. Proceed per docs/issues/0007-es8311-upstream-readiness.md (genericize → split PR → coordinate the successor)."
else
	echo "VERDICT: STILL HOLDING — ADR 0004 gate not met (need BOTH base #$BASE_PR merged AND a live successor PR). Keep waiting."
fi
