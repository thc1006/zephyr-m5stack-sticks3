#!/usr/bin/env bash
# Pre-push gate for Zephyr upstream pull requests.
#
# Run this BEFORE pushing, not after. On 2026-08-09 a pull request went out with a
# 76-character line in its commit body, Zephyr's gitlint rejects at 75, and CI went
# red for a reason that was fully catchable here. Monitoring after the push is the
# wrong layer: it structurally cannot beat GitHub's own notification.
#
#   scripts/zephyr_pr_gate.sh <worktree> [base]
#
#   worktree  a checkout with the commit to be pushed at HEAD
#   base      what to diff against, default origin/main
#
# Exits non-zero if anything fails, so it can gate a push:
#
#   scripts/zephyr_pr_gate.sh ../wt-fix && git -C ../wt-fix push ...
#
# Requires docker and the Zephyr CI image. v0.28.4 CANNOT be used: its venv has no
# jsonschema, so both twister and west build die in it. Only v0.29.2 works.
set -euo pipefail

IMAGE="${ZEPHYR_CI_IMAGE:-ghcr.io/zephyrproject-rtos/ci:v0.29.2}"
VOLUME="${ZEPHYR_WEST_VOLUME:-lvglws}"

WT="${1:?usage: $0 <worktree> [base]}"
BASE="${2:-origin/main}"

WT="$(cd "$WT" && pwd)"

# Deliberately NOT mktemp: under Git Bash on Windows /tmp is a path only the shell
# understands, so docker mounts something else and the container sees an empty
# directory. This script hit that on its own first run. Stage next to the worktree
# instead, and hand docker a native path when cygpath can produce one.
GATE="$WT/../.pr-gate.$$"
mkdir -p "$GATE"
trap 'rm -rf "$GATE"' EXIT

if command -v cygpath >/dev/null 2>&1; then
	GATE_MOUNT="$(cygpath -m "$GATE")"
else
	GATE_MOUNT="$(cd "$GATE" && pwd)"
fi

# EVERY commit in the range, not just HEAD. A push sends the whole series and CI checks
# the whole series. This script checked only HEAD until 2026-08-10, which meant a
# three-commit branch was gated on its last commit -- a six-line devicetree overlay --
# while 1450 lines of new driver went past checkpatch untouched, and it still printed
# "gate passed".
COMMITS=$(git -C "$WT" rev-list --reverse "$BASE..HEAD")
if [ -z "$COMMITS" ]; then
	echo "FAIL no commits in $BASE..HEAD"
	exit 1
fi
echo "== gate: $(echo "$COMMITS" | wc -l) commit(s), $BASE..$(git -C "$WT" rev-parse --short HEAD)"

# Cheap local checks first, so an obvious miss does not wait on a container.
i=0
for c in $COMMITS; do
	i=$((i + 1))
	SHORT=$(git -C "$WT" rev-parse --short "$c")
	git -C "$WT" log -1 --format=%B "$c" > "$GATE/msg-$i.txt"

	LONGEST=$(awk '{ print length }' "$GATE/msg-$i.txt" | sort -rn | head -1)
	if [ "$LONGEST" -gt 75 ]; then
		echo "FAIL $SHORT: message has a $LONGEST-character line; Zephyr's limit is 75"
		awk 'length > 75 { print "  " length ": " $0 }' "$GATE/msg-$i.txt"
		exit 1
	fi

	if grep -qiE 'co-authored-by: .*(claude|copilot|bot)|generated with' "$GATE/msg-$i.txt"; then
		echo "FAIL $SHORT: message carries a bot authorship or generated-with trailer"
		exit 1
	fi

	if ! git -C "$WT" log -1 --format='%(trailers:key=Signed-off-by)' "$c" | grep -q .; then
		echo "FAIL $SHORT: commit is missing its Signed-off-by trailer"
		exit 1
	fi

	echo "ok   $SHORT  longest line $LONGEST, signed off, no bot trailer"
done

# A push dismisses every approval on the branch, so knowing the vote count is part
# of deciding whether to push at all, not something to check afterwards. On
# 2026-08-11 a self-invented "commit message is too long" threshold nearly cost
# five pull requests their approvals, three of which had been earned that evening.
# Set ZEPHYR_GATE_ALLOW_DISMISS=1 when losing the votes is the actual intent.
BRANCH=$(git -C "$WT" rev-parse --abbrev-ref HEAD)
if command -v gh >/dev/null 2>&1; then
	if bash "$(dirname "$0")/zephyr_live_votes.sh" --branch "$BRANCH"; then
		:
	elif [ "${ZEPHYR_GATE_ALLOW_DISMISS:-0}" = "1" ]; then
		echo "     ZEPHYR_GATE_ALLOW_DISMISS=1 set; continuing anyway"
	else
		echo "FAIL pushing $BRANCH would dismiss the approvals listed above"
		exit 1
	fi
else
	echo "FAIL gh is not on PATH, so the vote check cannot run"
	echo "     refusing to pass a gate whose approval check did not execute"
	exit 1
fi

git -C "$WT" format-patch "$BASE..HEAD" -o "$GATE/patches" > /dev/null

# MSYS_NO_PATHCONV keeps Git Bash from mangling the -v arguments on Windows. Do not
# stage files under /tmp for the mount: in Git Bash that is a Windows path and the
# container silently sees something else.
MSYS_NO_PATHCONV=1 docker run --rm \
	-v "$VOLUME:/work" \
	-v "$GATE_MOUNT:/host" \
	"$IMAGE" bash -c '
	set -o pipefail
	cd /work/zephyr
	rc=0
	for m in /host/msg-*.txt; do
		if gitlint --config .gitlint --msg-filename "$m" 2>/dev/null; then
			echo "ok   gitlint $(basename "$m")"
		else
			echo "FAIL gitlint $(basename "$m")"
			gitlint --config .gitlint --msg-filename "$m" || true
			rc=1
		fi
	done
	for p in /host/patches/*.patch; do
		if ./scripts/checkpatch.pl --mailback --no-tree --patch "$p" > /host/cp.out 2>&1; then
			echo "ok   checkpatch $(basename "$p")"
		else
			echo "FAIL checkpatch $(basename "$p")"
			grep -E "ERROR|WARNING" /host/cp.out | head -20 || true
			rc=1
		fi
	done
	exit $rc
'

echo "== gate passed; safe to push"
