#!/usr/bin/env bash
# Copyright (c) 2026 Hsiu-Chi Tsai
# SPDX-License-Identifier: Apache-2.0
#
# Issue #7 monitor: report whether the gate to upstream the ES8311 codec driver
# has opened. Per ADR 0004 we HOLD until the board PR lands and the ES8311 work
# resumes on a live PR. This prints the live state of the gating PRs and a
# GATE OPEN / STILL HOLDING verdict. Needs `gh` authenticated.
#
# Usage: bash scripts/check_es8311_upstream_gate.sh
set -euo pipefail

Z=zephyrproject-rtos/zephyr
gate_open=0

field() { gh pr view "$1" --repo "$Z" --json "$2" -q ".$2" 2>/dev/null || echo "?"; }

echo "ES8311 upstream gate — $(date -u '+%Y-%m-%d %H:%MZ')"
echo "----------------------------------------------------"

for pr in 110205 107655 107660; do
	state=$(field "$pr" state)
	merged=$(field "$pr" mergedAt)
	review=$(field "$pr" reviewDecision)
	title=$(field "$pr" title)
	printf '#%-7s %-6s merged=%-10s review=%-18s %s\n' \
		"$pr" "$state" "${merged:-no}" "${review:-none}" "${title:0:48}"
	# Gate opens when the board PR (#110205, ours) or the base board (#107655) merges.
	if { [ "$pr" = "110205" ] || [ "$pr" = "107655" ]; } && [ "$merged" != "" ] && [ "$merged" != "no" ] && [ "$merged" != "null" ]; then
		gate_open=1
	fi
done

# Also: any NEW open ES8311 PR (a live successor to the stalled #107660) is a signal.
echo "--- live ES8311 PRs upstream ---"
gh search prs --repo "$Z" es8311 --state open --json number,author,updatedAt,title \
	-q '.[] | "#\(.number) \(.author.login) \(.updatedAt[0:10]) — \(.title[0:50])"' 2>/dev/null || echo "(search unavailable)"

echo "----------------------------------------------------"
if [ "$gate_open" = "1" ]; then
	echo "VERDICT: GATE OPEN — a gating board PR has merged. Proceed per docs/issues/0007-es8311-upstream-readiness.md (genericize → split PR → coordinate #107660)."
else
	echo "VERDICT: STILL HOLDING — no gating board PR merged yet. Keep waiting (ADR 0004)."
fi
