#!/usr/bin/env bash
# Copyright (c) 2026 Hsiu-Chi Tsai
# SPDX-License-Identifier: Apache-2.0
#
# Offline test for scripts/check_es8311_upstream_gate.sh. A mock `gh` and a mock
# readiness check go on PATH, so nothing here touches the network or the tree.
#
# Both the printed verdict AND the exit status are asserted. Checking only the
# text would miss a script that says HOLD and exits 0, which is what the first
# version did: a verdict that lives only in stdout cannot be acted on.
#
#   0 GO   1 HOLD   2 ENGAGE   3 ALREADY_OPEN   4 PREP
#
# The scenarios are the ways this gate has been, or could be, wrong:
#
#   ours_open  We open the real ES8311 driver PR. The old gate excluded every PR
#              authored by us, so it found ours, discarded it, and said GO again:
#              "submit your own PR" - a second one. This is the bug that made the
#              rewrite necessary.
#   saturated  gh search returns exactly the limit. A truncated result set is not
#              evidence of absence and must not be read as a clear lane.
#   notready   The lane is clear but the code is not. Reporting that as GO is what
#              let a readiness checklist claim "checkpatch clean" for a month while
#              the driver carried a 137-column line.
#   ghdown     gh cannot answer at all.
#   searchfail The search specifically fails while pr view still works.
#   engage     Somebody else has an open ES8311 PR.
#   clear      Nothing open, code ready.
#
# Usage: bash scripts/test_check_es8311_gate.sh
set -uo pipefail

here=$(cd "$(dirname "$0")" && pwd)
gate="$here/check_es8311_upstream_gate.sh"
td=$(mktemp -d)
trap 'rm -rf "$td"' EXIT
mkdir -p "$td/bin"

cat > "$td/bin/gh" <<'MOCK'
#!/usr/bin/env bash
# Fake gh; $SCEN selects the scenario.
prview() {
  [ "$SCEN" = "ghdown" ] && return 1
  echo '{"state":"MERGED","mergedAt":"2026-06-11T21:14:01Z","reviewDecision":"APPROVED","title":"fake"}'
}
search() {
  [ "$SCEN" = "ghdown" ] && return 1
  [ "$SCEN" = "searchfail" ] && return 1
  case "$SCEN" in
    engage)
      printf '109999\tsomebody\t2026-07-10\tES8311 codec driver, take 2\n'
      printf '110205\tthc1006\t2026-07-11\tM5Stack StickS3\n'
      ;;
    ours_open)
      # The real ES8311 driver PR, opened by us. NOT the board PR.
      printf '123456\tthc1006\t2026-07-20\tdrivers: audio: add Everest ES8311 codec driver\n'
      printf '110205\tthc1006\t2026-07-11\tM5Stack StickS3\n'
      ;;
    saturated)
      # Exactly the limit the gate asks for: the result set is truncated.
      for i in $(seq 1 100); do printf '%d\tsomebody\t2026-07-10\tnoise\n' "$((900000 + i))"; done
      ;;
    *)
      printf '110205\tthc1006\t2026-07-11\tM5Stack StickS3\n'
      ;;
  esac
  return 0
}
case "$1 $2" in
  "pr view")   prview ;;
  "search prs") search ;;
  *) exit 0 ;;
esac
MOCK
chmod +x "$td/bin/gh"

# jq is used by the gate to read the single-shot PR snapshot.
command -v jq >/dev/null 2>&1 || {
	echo "SKIP: jq is not installed, and the gate parses its PR snapshot with it"
	exit 0
}

printf '#!/usr/bin/env bash\nexit 0\n' > "$td/ready_ok.sh"
printf '#!/usr/bin/env bash\necho "  FAIL  mock: the code is not ready"\nexit 1\n' > "$td/ready_bad.sh"
chmod +x "$td/ready_ok.sh" "$td/ready_bad.sh"

pass=0 fail=0
check() { # SCEN READINESS EXPECT_REGEX EXPECT_RC label
	local out verdict rc ready
	ready="$td/ready_ok.sh"
	[ "$2" = "notready" ] && ready="$td/ready_bad.sh"

	out=$(SCEN="$1" ES8311_READINESS_CHECK="$ready" PATH="$td/bin:$PATH" bash "$gate") \
		&& rc=0 || rc=$?
	verdict=$(echo "$out" | grep VERDICT)

	if echo "$verdict" | grep -qE "$3" && [ "$rc" = "$4" ]; then
		printf 'PASS  %-11s rc=%s  %s\n' "$1" "$rc" "$5"
		pass=$((pass + 1))
	else
		printf 'FAIL  %-11s %s\n      want: %s rc=%s\n      got : %s rc=%s\n' \
			"$1" "$5" "$3" "$4" "$verdict" "$rc"
		fail=$((fail + 1))
	fi
}

check clear      ready    'VERDICT: GO'           0 'lane clear + code ready -> submit'
check clear      notready 'VERDICT: PREP'         4 'lane clear + code NOT ready -> not GO'
check ours_open  ready    'VERDICT: ALREADY_OPEN' 3 'our own driver PR is open -> do not open a second'
check engage     ready    'VERDICT: ENGAGE'       2 'a third party has one open -> do not compete'
check saturated  ready    'VERDICT: HOLD'         1 'search truncated -> absence is not proven'
check ghdown     ready    'VERDICT: HOLD'         1 'gh down -> never act on missing data'
check searchfail ready    'VERDICT: HOLD'         1 'search failed -> never act on missing data'

echo "----------------------------------------"
echo "PASS=$pass FAIL=$fail"
[ "$fail" -eq 0 ]
