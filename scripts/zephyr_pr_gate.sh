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

echo "== gate: $(git -C "$WT" rev-parse --short HEAD) against $BASE"

# Cheap local checks first, so an obvious miss does not wait on a container.
LONGEST=$(git -C "$WT" log -1 --format=%B | awk '{ print length }' | sort -rn | head -1)
if [ "$LONGEST" -gt 75 ]; then
	echo "FAIL commit body has a $LONGEST-character line; Zephyr's limit is 75"
	git -C "$WT" log -1 --format=%B | awk 'length > 75 { print "  " length ": " $0 }'
	exit 1
fi
echo "ok   longest commit-message line is $LONGEST"

if git -C "$WT" log -1 --format=%B | grep -qiE 'co-authored-by: .*(claude|copilot|bot)|generated with'; then
	echo "FAIL commit message carries a bot authorship or generated-with trailer"
	exit 1
fi
echo "ok   no bot authorship trailer"

if ! git -C "$WT" log -1 --format='%(trailers:key=Signed-off-by)' | grep -q .; then
	echo "FAIL commit is missing its Signed-off-by trailer"
	exit 1
fi
echo "ok   Signed-off-by present"

git -C "$WT" log -1 --format=%B > "$GATE/msg.txt"
git -C "$WT" format-patch -1 --stdout > "$GATE/patch.diff"

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
	if gitlint --config .gitlint --msg-filename /host/msg.txt 2>/dev/null; then
		echo "ok   gitlint"
	else
		echo "FAIL gitlint"
		gitlint --config .gitlint --msg-filename /host/msg.txt || true
		rc=1
	fi
	if ./scripts/checkpatch.pl --mailback --no-tree --patch /host/patch.diff > /host/cp.out 2>&1; then
		echo "ok   checkpatch"
	else
		echo "FAIL checkpatch"
		grep -E "ERROR|WARNING" /host/cp.out | head -20 || true
		rc=1
	fi
	exit $rc
'

echo "== gate passed; safe to push"
