#!/usr/bin/env bash
# Copyright (c) 2026 Hsiu-Chi Tsai
# SPDX-License-Identifier: Apache-2.0
#
# Mechanically verify the ticks in docs/issues/0007-es8311-upstream-readiness.md.
#
# This exists because a readiness checklist is a CLAIM ABOUT THE CODE, and CI does
# not check claims. The checklist recorded the driver as "checkpatch clean" on
# 2026-06-11, and commit 4fa40be introduced a 137-column line in the same week.
# The tick was written once and never re-checked, PR #16 and PR #22 both merged
# green with it in place, and the claim stayed false for a month. A tick a human
# writes decays; a tick a script checks does not.
#
# Everything below is one of those ticks, turned into something that can fail.
#
# Usage: bash scripts/check_es8311_readiness.sh
# Exit:  0 = every claim holds, 1 = at least one does not.
set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

DRIVER=drivers/audio/es8311.c
EMUL=drivers/audio/emul_es8311.c
KCONFIG=drivers/audio/Kconfig.es8311
BINDING=dts/bindings/audio/everest,es8311.yaml
TEST=tests/drivers/audio/es8311/src/main.c
DOC=docs/issues/0007-es8311-upstream-readiness.md

ALL=("$DRIVER" "$EMUL" "$KCONFIG" "$BINDING" "$TEST")

fail=0
ok()   { printf '  ok    %s\n' "$1"; }
bad()  { printf '  FAIL  %s\n' "$1"; fail=1; }

echo "ES8311 upstream readiness (mechanical check of docs/issues/0007)"
echo "----------------------------------------------------------------"

# 1. checkpatch's hard limit. This is the one that was silently false for a month.
long=$(awk 'length > 100 {print FILENAME":"FNR" ("length" cols)"}' "${ALL[@]}")
if [ -z "$long" ]; then
	ok "no line over 100 columns (checkpatch LONG_LINE)"
else
	bad "lines over 100 columns:"
	printf '        %s\n' $long
fi

# 2. Pure ASCII. A stray non-ASCII byte is an instant upstream review comment.
nonascii=$(LC_ALL=C grep -lnP '[^\x00-\x7F]' "${ALL[@]}" 2>/dev/null)
if [ -z "$nonascii" ]; then
	ok "pure ASCII"
else
	bad "non-ASCII bytes in: $nonascii"
fi

# 3. No trailing whitespace.
trail=$(grep -lnE ' +$' "${ALL[@]}" 2>/dev/null)
if [ -z "$trail" ]; then
	ok "no trailing whitespace"
else
	bad "trailing whitespace in: $trail"
fi

# 4. The driver must carry no trace of this project. Anything here reads to a
#    maintainer as a vendor blob dropped into the tree.
refs=$(grep -niE 'M5Stack|M5Stick|StickS3|HW-0[0-9]|ESP-ADF|esp-bsp|esp_codec_dev|M5GFX|M5Unified|AW8737|M5PM1|TODO|FIXME|XXX' \
	"$DRIVER" "$KCONFIG" "$BINDING" 2>/dev/null)
if [ -z "$refs" ]; then
	ok "driver, Kconfig and binding are free of project-specific references"
else
	bad "project-specific references:"
	printf '        %s\n' "$refs"
fi

# 5. No AI footers anywhere. Non-negotiable.
ai=$(grep -niE 'Co-Authored-By|Generated with|Claude|Anthropic|Copilot' "${ALL[@]}" 2>/dev/null)
if [ -z "$ai" ]; then
	ok "no AI footers or bot authorship"
else
	bad "AI/bot tells:"
	printf '        %s\n' "$ai"
fi

# 6. SPDX and copyright on every file.
for f in "${ALL[@]}"; do
	if ! head -5 "$f" | grep -q 'SPDX-License-Identifier: Apache-2.0'; then
		bad "$f: missing SPDX-License-Identifier"
	fi
	if ! head -5 "$f" | grep -q 'Copyright (c)'; then
		bad "$f: missing copyright line"
	fi
done
ok "SPDX and copyright headers (see any FAIL above)"

# 7. The driver is generic: it binds on the compatible, not on a board.
if grep -q '^#define DT_DRV_COMPAT everest_es8311' "$DRIVER"; then
	ok "DT_DRV_COMPAT everest_es8311 (board-independent binding)"
else
	bad "the driver does not bind on everest_es8311"
fi

# 8. The test count the doc claims must be the test count that exists. This is
#    what stops "11/11" outliving the eleventh test.
have=$(grep -cE '^ZTEST\(es8311,' "$TEST")
claim=$(grep -oE 'twister native_sim \*\*[0-9]+/[0-9]+\*\*' "$DOC" | grep -oE '[0-9]+' | head -1)
if [ -z "$claim" ]; then
	bad "the readiness doc no longer states a test count in the form it is parsed from"
elif [ "$have" = "$claim" ]; then
	ok "the doc claims $claim ztest cases and $have exist"
else
	bad "the doc claims $claim ztest cases but $have exist"
fi

# 9. The audio codec is a driver class since zephyr#110631. This tree now tracks a
#    Zephyr that has it, so the in-repo copy and the upstream copy are the same
#    file and must both use DEVICE_API(audio_codec, ...).
if grep -qE '^static DEVICE_API\(audio_codec' "$DRIVER"; then
	ok "the in-repo copy registers through the audio_codec driver class"
else
	bad "the in-repo copy does not use DEVICE_API(audio_codec, ...)"
fi

echo "----------------------------------------------------------------"
if [ "$fail" = 0 ]; then
	echo "READY: every mechanical claim in docs/issues/0007 holds."
else
	echo "NOT READY: at least one claim in docs/issues/0007 is false. Fix the code"
	echo "or fix the claim, but do not merge with them disagreeing."
fi
exit "$fail"
