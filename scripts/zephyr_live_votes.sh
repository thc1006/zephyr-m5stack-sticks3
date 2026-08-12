#!/usr/bin/env bash
# Report the review votes a push would destroy, and refuse to guess.
#
# Zephyr dismisses every existing approval when a branch is force-pushed. On
# 2026-08-12 a scan of thirteen pull requests reported "no votes" for all of
# them because the query used `gh api --jq --arg`, which gh does not accept.
# The error went to stderr, the loop substituted an empty string, and every
# row rendered as clean. Four of those pull requests held approvals. A broken
# probe that renders as "safe to push" is worse than no probe.
#
# So this script cross-checks two independent endpoints:
#
#   1. pulls/N/reviews   -- individual reviews, with the commit each sits on
#   2. reviewDecision    -- GitHub's own computed verdict for the PR
#
# If they disagree, the probe is broken and the script fails. It never reports
# "clean" on a query it could not run.
#
#   scripts/zephyr_live_votes.sh <pr-number>...
#   scripts/zephyr_live_votes.sh --branch <branch>
#
# Exit status:
#   0  no approval sits on the current head; a force-push destroys nothing
#   1  an approval sits on the current head, or the two endpoints disagree
set -euo pipefail

REPO="${ZEPHYR_REPO:-zephyrproject-rtos/zephyr}"

usage() { echo "usage: $0 <pr-number>... | --branch <branch>" >&2; exit 2; }

[ $# -ge 1 ] || usage

if [ "$1" = "--branch" ]; then
	[ $# -eq 2 ] || usage
	PRS=$(gh pr list --repo "$REPO" --head "$2" --state open --json number --jq '.[].number')
	if [ -z "$PRS" ]; then
		echo "no open PR for branch $2; nothing to lose"
		exit 0
	fi
else
	PRS="$*"
fi

rc=0

for n in $PRS; do
	head=$(gh api "repos/$REPO/pulls/$n" --jq .head.sha)

	# Endpoint 1: every review, with the commit it was left on. --arg belongs
	# to jq, not to gh; passing it to gh is what broke this on 2026-08-12.
	reviews=$(gh api "repos/$REPO/pulls/$n/reviews" --paginate)

	on_head=$(printf '%s' "$reviews" | jq -r --arg h "$head" \
		'[.[] | select(.state == "APPROVED" and .commit_id == $h)] | length')
	approvers=$(printf '%s' "$reviews" | jq -r --arg h "$head" \
		'[.[] | select(.state == "APPROVED" and .commit_id == $h) | .user.login]
		 | unique | join(", ")')
	blocked=$(printf '%s' "$reviews" | jq -r \
		'[.[] | select(.state == "CHANGES_REQUESTED") | .user.login] | unique | join(", ")')

	# Endpoint 2: GitHub's own verdict, computed independently of the above.
	decision=$(gh pr view "$n" --repo "$REPO" --json reviewDecision --jq '.reviewDecision // "NONE"')

	# The control. These two are derived differently, so a disagreement means
	# the query shape is wrong rather than that the world is quiet.
	if [ "$decision" = "APPROVED" ] && [ "$on_head" -eq 0 ]; then
		echo "FAIL #$n: reviewDecision says APPROVED but the review scan found none on $head"
		echo "          the probe is broken; do not read this as 'no votes'"
		rc=1
		continue
	fi
	if [ "$decision" = "CHANGES_REQUESTED" ] && [ -z "$blocked" ]; then
		echo "FAIL #$n: reviewDecision says CHANGES_REQUESTED but the scan found none"
		echo "          the probe is broken; do not read this as 'no votes'"
		rc=1
		continue
	fi

	if [ "$on_head" -gt 0 ]; then
		echo "FAIL #$n: $on_head approval(s) sit on the current head ($approvers)"
		echo "          a push DISMISSES them; re-earning a vote costs days"
		rc=1
	else
		printf 'ok   #%s: no approval on head' "$n"
		[ -n "$blocked" ] && printf ' (changes requested by %s, on an older commit)' "$blocked"
		printf '\n'
	fi
done

exit $rc
