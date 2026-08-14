#!/usr/bin/env bash
# Check a claim of the form "<needle> does not appear anywhere in the tree".
#
#   scripts/zephyr_absence_check.sh <needle> <control> [repo]
#
# Three times in two days a claim like that went out after a search that covered
# only part of the tree: dts/ and boards/ but not tests/ (published as fact in
# zephyr#116336, then publicly corrected), boards/ but not socs/, and once with
# no search at all. Knowing the rule did not prevent the third one, so the rule
# is a script now.
#
# Two properties are the whole point:
#
#   - the search takes NO path arguments. git grep walks the entire tracked
#     tree, so a directory cannot be forgotten.
#   - <control> is mandatory and must be FOUND. A needle that returns zero and
#     a search that is broken look identical; the control is what tells them
#     apart. Pick a control the same shape as the needle, in the same files you
#     expect the needle would live in if it existed.
#
# Exit status: 0 the needle is absent and the control proves the search ran;
#              1 the needle is present, so the claim is false;
#              2 the control found nothing, so the result means nothing.
set -euo pipefail

NEEDLE="${1:?usage: $0 <needle> <control> [repo]}"
CONTROL="${2:?a positive control is mandatory: pick a string you expect to be FOUND}"
REPO="${3:-.}"

cd "$REPO"
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
	echo "FAIL: $PWD is not a git work tree"
	exit 2
}

# -F: fixed string, so needles containing & < > * are taken literally.
# No pathspec is passed, deliberately.
needle_hits=$(git grep -F -c -- "$NEEDLE" | wc -l || true)
control_hits=$(git grep -F -c -- "$CONTROL" | wc -l || true)

echo "  tree     : $(git rev-parse --short HEAD) at $PWD"
echo "  needle   : $NEEDLE"
echo "  control  : $CONTROL   -> $control_hits file(s)"

if [ "$control_hits" -eq 0 ]; then
	echo "INCONCLUSIVE: the control found nothing, so a zero for the needle proves nothing."
	echo "              pick a control that exists, then run again."
	exit 2
fi

if [ "$needle_hits" -ne 0 ]; then
	echo "FALSE: the needle is present in $needle_hits file(s):"
	git grep -F -l -- "$NEEDLE" | sed 's/^/    /'
	echo "       do not claim it is absent."
	exit 1
fi

echo "ABSENT: 0 file(s), and the control proves the search reached the tree."
