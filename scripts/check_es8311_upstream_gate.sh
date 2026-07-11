#!/usr/bin/env bash
# Copyright (c) 2026 Hsiu-Chi Tsai
# SPDX-License-Identifier: Apache-2.0
#
# Issue #7: should we submit the ES8311 codec driver upstream right now?
#
# The answer has TWO independent parts, and the earlier versions of this script
# conflated them:
#
#   COORDINATION - is the upstream lane clear? Is somebody else already driving an
#                  ES8311 driver, or have WE already opened one?
#   READINESS    - is the code actually fit to submit? That is not a question about
#                  GitHub at all, and it is answered by check_es8311_readiness.sh.
#
# "No competing PR was found" was previously reported as GO, which said nothing
# about whether the driver was ready. It was not: the readiness checklist claimed
# the driver was checkpatch-clean while it carried a 137-column line.
#
# Two further bugs this version fixes, both of the same family:
#
#   * It excluded every PR authored by us, so once WE opened the real ES8311
#     driver PR the search would find it, discard it, and say GO again: "submit
#     your own PR" - a second one. Our board PR #110205 is the only PR of ours
#     that is genuinely not an ES8311 driver, and it is excluded by number.
#   * `gh search prs` defaults to 30 results and searches title, body and comments
#     rather than the diff. A saturated result set is not evidence of absence, so
#     it is now reported as UNKNOWN rather than quietly read as "clear".
#
# Exit status is part of the contract:
#   0  GO           the lane is clear and the code is ready
#   1  HOLD         the search could not answer; never act on missing data
#   2  ENGAGE       somebody else has an open ES8311 PR: join it, do not compete
#   3  ALREADY_OPEN we have one open already: maintain it, do not open another
#   4  PREP         the lane is clear but the code is not ready yet
#
# Usage: bash scripts/check_es8311_upstream_gate.sh
set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

Z=zephyrproject-rtos/zephyr
BASE_PR=107655                      # ESP32-S3-BOX-3: context, not a precondition
CLOSED_PR=107660                    # the original ES8311 PR, closed 2026-06-12
OUR_BOARD_PR=110205                 # ours, but a board PR, not an ES8311 driver
OUR_AUTHORS=(thc1006 junnncct1106)
SEARCH_LIMIT=100

# The search covers title, body and comments, not the diff, so cast a wider net
# than one spelling of the part number.
SEARCH_TERMS=("es8311" "everest,es8311" "Everest Semiconductor codec")

echo "ES8311 upstream gate - $(date -u '+%Y-%m-%d %H:%MZ')"
echo "===================================================================="

# ---------------------------------------------------------------- coordination
#
# One gh call per PR, not four. Every field comes from the same snapshot, so the
# state cannot change underneath the script between questions about it.
pr_json() { gh pr view "$1" --repo "$Z" --json state,mergedAt,reviewDecision,title 2>/dev/null; }

echo "-- the PRs this decision has a history with --"
for pr in "$BASE_PR" "$CLOSED_PR" "$OUR_BOARD_PR"; do
	j=$(pr_json "$pr")
	if [ -z "$j" ]; then
		printf '  #%-7s (gh could not answer)\n' "$pr"
		continue
	fi
	printf '  #%-7s %-7s merged=%-21s %s\n' "$pr" \
		"$(echo "$j" | jq -r '.state')" \
		"$(echo "$j" | jq -r '.mergedAt // "no"')" \
		"$(echo "$j" | jq -r '.title[0:52]')"
done

echo
echo "-- open ES8311 pull requests upstream --"

found=""
search_ok=1
saturated=0

for term in "${SEARCH_TERMS[@]}"; do
	out=$(gh search prs --repo "$Z" "$term" --state open --limit "$SEARCH_LIMIT" \
		--json number,author,updatedAt,title \
		-q '.[] | "\(.number)\t\(.author.login // "?")\t\(.updatedAt[0:10])\t\(.title[0:48])"' \
		2>/dev/null)
	if [ $? -ne 0 ]; then
		search_ok=0
		echo "  !! the search for '$term' did not run"
		continue
	fi
	n=$(printf '%s' "$out" | grep -c . || true)
	if [ "$n" -ge "$SEARCH_LIMIT" ]; then
		saturated=1
		echo "  !! the search for '$term' returned $n results, the limit. It is"
		echo "     truncated, so absence cannot be concluded from it."
	fi
	found=$(printf '%s\n%s' "$found" "$out")
done

ours_open=0
third_party=0

if [ "$search_ok" = 1 ] && [ "$saturated" = 0 ]; then
	while IFS=$'\t' read -r num author updated title; do
		[ -z "${num:-}" ] && continue

		# Our board PR names the part but does not implement it.
		if [ "$num" = "$OUR_BOARD_PR" ]; then
			printf '     #%-7s %-14s %s  (our board PR, not a driver)\n' \
				"$num" "$author" "$updated"
			continue
		fi

		mine=0
		for a in "${OUR_AUTHORS[@]}"; do
			[ "$a" = "$author" ] && mine=1
		done

		if [ "$mine" = 1 ]; then
			ours_open=1
			printf '  >> #%-7s %-14s %s  %s  (OURS, already open)\n' \
				"$num" "$author" "$updated" "$title"
		else
			third_party=1
			printf '  ** #%-7s %-14s %s  %s  (someone else)\n' \
				"$num" "$author" "$updated" "$title"
		fi
	done <<< "$(printf '%s' "$found" | sort -u)"

	if [ "$ours_open" = 0 ] && [ "$third_party" = 0 ]; then
		echo "     none"
	fi
fi

if [ "$search_ok" = 0 ] || [ "$saturated" = 1 ]; then
	coordination=UNKNOWN
elif [ "$ours_open" = 1 ]; then
	coordination=ALREADY_OPEN
elif [ "$third_party" = 1 ]; then
	coordination=THIRD_PARTY
else
	coordination=CLEAR
fi

# ------------------------------------------------------------------- readiness
#
# Overridable so the offline test can drive this branch without depending on the
# state of the real tree.
READINESS=${ES8311_READINESS_CHECK:-scripts/check_es8311_readiness.sh}

echo
echo "-- technical readiness --"
if bash "$READINESS" > /dev/null 2>&1; then
	readiness=READY
	echo "  the mechanical claims in docs/issues/0007 all hold"
	echo "  (run $READINESS to see them)"
else
	readiness=NOT_READY
	echo "  $READINESS FAILS. The lane being clear does not make the code ready,"
	echo "  and those two were conflated before:"
	bash "$READINESS" 2>&1 | grep FAIL | sed 's/^/    /'
fi

# --------------------------------------------------------------------- verdict
echo
echo "===================================================================="
printf 'coordination : %s\n' "$coordination"
printf 'readiness    : %s\n' "$readiness"
echo

case "$coordination" in
UNKNOWN)
	echo "VERDICT: HOLD - the searches could not answer, or were truncated."
	echo "  Absence of evidence is not evidence of absence. Re-run when gh works."
	exit 1
	;;
ALREADY_OPEN)
	echo "VERDICT: ALREADY_OPEN - we have an ES8311 pull request open upstream."
	echo "  Maintain it. Do NOT open a second one."
	exit 3
	;;
THIRD_PARTY)
	echo "VERDICT: ENGAGE - somebody else has an open ES8311 pull request (**)."
	echo "  Do not compete with it. Engage it per ADR 0004 and offer the"
	echo "  hardware-verified capture route as a follow-up."
	exit 2
	;;
esac

if [ "$readiness" != READY ]; then
	echo "VERDICT: PREP - no matching open pull request was found by the configured"
	echo "  searches, so the lane appears clear, but the code is not ready. Fix what"
	echo "  scripts/check_es8311_readiness.sh reports, then run this again."
	exit 4
fi

echo "VERDICT: GO - no matching open pull request was found, and every mechanical"
echo "  readiness claim holds."
echo "  Submit the codec driver, Kconfig.es8311, the everest,es8311 binding and a"
echo "  node in tests/drivers/build_all/audio/i2c_devices.overlay. NOT the ztest"
echo "  and NOT the emulator: no in-tree codec ships either, so both are new"
echo "  surface in an area that has no maintainer. Offer them as a follow-up."
echo "  See docs/issues/0007-es8311-upstream-readiness.md."
exit 0
