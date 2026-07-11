#!/usr/bin/env bash
# Copyright (c) 2026 Hsiu-Chi Tsai
# SPDX-License-Identifier: Apache-2.0
#
# Issue #7 monitor: what should we do about upstreaming the ES8311 codec driver?
# Two facts decide it:
#   (1) has the base board PR #107655 MERGED?  -> an in-tree consumer exists
#   (2) is anyone else upstream driving an ES8311 PR right now?
#
#   (1) no             -> HOLD   : no in-tree consumer yet, wait.
#   (1) yes + (2) yes  -> ENGAGE : a live effort exists. Do not open a competing
#                                  driver (ADR 0004); engage it and offer our
#                                  HW-verified capture route as a follow-up.
#   (1) yes + (2) no   -> GO     : nobody is driving it. Submit our own clean
#                                  split PR (codec + binding + ztest).
#
# The first version of this gate required a live *successor* PR to appear before
# we acted. That deadlocks: if the upstream effort is abandoned (which is exactly
# what happened on 2026-06-12, see ADR 0004's 2026-07-11 update) no successor will
# ever appear, and the gate never opens. "Nobody is doing it" is a reason to GO,
# not a reason to wait forever.
#
# Fails safe in both directions. A gh outage / auth gap / rate limit degrades to
# HOLD: a `pr view` that returns nothing is never read as "merged", and a search
# that could not run is never read as "nobody is driving it".
#
# Usage: bash scripts/check_es8311_upstream_gate.sh
set -euo pipefail

Z=zephyrproject-rtos/zephyr
BASE_PR=107655                       # in-tree consumer: the ESP32-S3-BOX-3 board
CLOSED_PR=107660                     # the original ES8311 PR; closed 2026-06-12
OUR_PRS=(110205)                     # our own PR(s) - never a competing effort
OUR_AUTHORS=(thc1006 junnncct1106)   # our authors - their es8311 PRs are not competition

# field PR JSON_FIELD -> value, or "" on ANY gh error.
# Returns empty (not "?") on failure so a gh outage / auth gap / rate-limit can
# never be mistaken for a real value such as a false "merged" timestamp.
field() { gh pr view "$1" --repo "$Z" --json "$2" -q ".$2" 2>/dev/null || true; }

# is_merged PR -> success only if the PR is *actually* merged.
# Authoritative signal is state==MERGED; mergedAt must also be a real ISO-8601
# timestamp. "", "null" and "?" all fail.
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

for pr in "$BASE_PR" "$CLOSED_PR" "${OUR_PRS[@]}"; do
	state=$(field "$pr" state)
	merged=$(field "$pr" mergedAt)
	review=$(field "$pr" reviewDecision)
	title=$(field "$pr" title)
	printf '#%-7s %-6s merged=%-21s review=%-18s %s\n' \
		"$pr" "${state:-?}" "${merged:-no}" "${review:-none}" "${title:0:48}"
done

# (1) in-tree consumer: the base board must be MERGED.
base_merged=0
if is_merged "$BASE_PR"; then base_merged=1; fi

# (2) is anyone else driving ES8311 upstream? Any OPEN es8311 PR that is neither
# the closed original nor one of ours. The search must SUCCEED to count: `gh
# search` exits 0 with empty output when there genuinely are no matches, and
# non-zero when it could not run (rate limit, auth, outage). Only a successful
# search may be read as "nobody is driving it" - otherwise we would GO on missing
# data, which is the mirror image of the old false-merge bug.
echo "--- live ES8311 PRs upstream ---"
competing=0
search_ok=0
if prs=$(gh search prs --repo "$Z" es8311 --state open \
	--json number,author,updatedAt,title \
	-q '.[] | "\(.number)\t\(.author.login)\t\(.updatedAt[0:10])\t\(.title[0:50])"' 2>/dev/null); then
	search_ok=1
	while IFS=$'\t' read -r num author updated title; do
		[ -z "$num" ] && continue
		mark=' '
		if ! in_list "$num" "$CLOSED_PR" "${OUR_PRS[@]}" \
			&& ! in_list "$author" "${OUR_AUTHORS[@]}"; then
			mark='*'; competing=1
		fi
		printf ' %s #%-7s %-14s %s — %s\n' "$mark" "$num" "$author" "$updated" "${title:0:50}"
	done <<< "$prs"
	if [ "$competing" = 1 ]; then
		echo " (* = live competing ES8311 effort)"
	elif [ -z "$prs" ]; then
		echo " (none)"
	fi
else
	echo " !! gh search failed - cannot tell whether anyone is driving ES8311"
fi

if [ "$search_ok" = 0 ]; then
	driving=unknown
elif [ "$competing" = 1 ]; then
	driving=yes
else
	driving=no
fi

if [ "$base_merged" = 1 ]; then
	consumer=yes
else
	consumer=no
fi

echo "----------------------------------------------------"
printf 'in-tree consumer (#%s merged) : %s\n' "$BASE_PR" "$consumer"
printf 'someone else driving ES8311      : %s\n' "$driving"

if [ "$base_merged" = 0 ]; then
	echo "VERDICT: HOLD — the base board #$BASE_PR has not merged yet."
	echo "  No in-tree consumer to hang a codec driver on. Wait."
elif [ "$search_ok" = 0 ]; then
	echo "VERDICT: HOLD — the upstream search did not run."
	echo "  So \"nobody is driving ES8311\" is unproven. Never GO on missing data."
	echo "  Re-run when gh works."
elif [ "$competing" = 1 ]; then
	echo "VERDICT: ENGAGE — a live ES8311 PR exists upstream (marked * above)."
	echo "  Do NOT open a competing driver. Engage that PR per ADR 0004 and offer"
	echo "  the HW-verified capture/ADC route as a follow-up."
else
	echo "VERDICT: GO — the base board landed, nobody upstream is driving ES8311."
	echo "  Submit our own clean split PR (codec + everest,es8311 binding + ztest)"
	echo "  per docs/issues/0007-es8311-upstream-readiness.md."
fi
