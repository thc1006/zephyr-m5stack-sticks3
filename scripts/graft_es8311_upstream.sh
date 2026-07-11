#!/usr/bin/env bash
# Copyright (c) 2026 Hsiu-Chi Tsai
# SPDX-License-Identifier: Apache-2.0
#
# Plant the ES8311 driver into a real Zephyr tree exactly as the upstream PR will,
# so that "it compiles against upstream main" is something a machine re-runs rather
# than something a commit message asserts.
#
# That claim was made from one run, on one laptop, that nothing could reproduce:
# this repo's CI pins v4.4.0, and the driver's UPSTREAM form -- the one that would
# actually be reviewed, with the DEVICE_API declaration that does not exist on
# v4.4.0 -- was never built by anything, ever. An upstream API change would have
# turned the claim false with no test anywhere going red.
#
# It adds exactly what the PR adds, and nothing else:
#
#   drivers/audio/es8311.c                             generated (upstream API form)
#   drivers/audio/Kconfig.es8311                       generated (no OOT depends)
#   drivers/audio/Kconfig                              one source line, keep-sorted
#   drivers/audio/CMakeLists.txt                       one sources line, keep-sorted
#   dts/bindings/audio/everest,es8311.yaml             verbatim
#   tests/drivers/build_all/audio/i2c_devices.overlay  one node
#
# The keep-sorted blocks are CI-enforced upstream, so the lines go in sorted
# position (after da7212, before max98091) rather than at the end.
#
# Usage: bash scripts/graft_es8311_upstream.sh <zephyr-tree>
set -euo pipefail

Z=${1:?usage: graft_es8311_upstream.sh <zephyr-tree>}
here=$(cd "$(dirname "$0")/.." && pwd)

[ -f "$Z/drivers/audio/Kconfig" ] || { echo "not a Zephyr tree: $Z" >&2; exit 1; }

echo "-- generating the upstream form of the driver --"
python3 "$here/scripts/sync_es8311_upstream.py" "$Z/drivers/audio"

echo "-- binding --"
cp "$here/dts/bindings/audio/everest,es8311.yaml" "$Z/dts/bindings/audio/"

echo "-- Kconfig + CMakeLists, in sorted position --"
# Idempotent: re-running must not add the line twice.
if ! grep -q 'Kconfig.es8311' "$Z/drivers/audio/Kconfig"; then
	sed -i 's|^source "drivers/audio/Kconfig.max98091"|source "drivers/audio/Kconfig.es8311"\nsource "drivers/audio/Kconfig.max98091"|' \
		"$Z/drivers/audio/Kconfig"
fi
if ! grep -q 'es8311.c' "$Z/drivers/audio/CMakeLists.txt"; then
	sed -i 's|^zephyr_library_sources_ifdef(CONFIG_AUDIO_CODEC_MAX98091 max98091.c)|zephyr_library_sources_ifdef(CONFIG_AUDIO_CODEC_ES8311 es8311.c)\nzephyr_library_sources_ifdef(CONFIG_AUDIO_CODEC_MAX98091 max98091.c)|' \
		"$Z/drivers/audio/CMakeLists.txt"
fi

grep -q 'Kconfig.es8311' "$Z/drivers/audio/Kconfig" ||
	{ echo "FAIL: the Kconfig source line did not land (did the sorted anchor move?)" >&2; exit 1; }
grep -q 'es8311.c' "$Z/drivers/audio/CMakeLists.txt" ||
	{ echo "FAIL: the CMakeLists line did not land (did the sorted anchor move?)" >&2; exit 1; }

echo "-- the build_all node: the only thing that makes CI compile a codec at all --"
ov=$Z/tests/drivers/build_all/audio/i2c_devices.overlay
if ! grep -q 'everest,es8311' "$ov"; then
	# Append inside test_i2c, before the three closing braces that end the file.
	python3 - "$ov" <<'PY'
import io, sys

path = sys.argv[1]
src = io.open(path, encoding="utf-8", newline="\n").read()

node = """
			test_i2c_es8311: es8311@4 {
				compatible = "everest,es8311";
				status = "okay";
				reg = <0x4>;
			};
"""

# The last '		};' closes test_i2c. Insert the node just before it.
anchor = "\t\t};\n\t};\n};\n"
if src.count(anchor) != 1:
    sys.exit("the overlay does not end the way this script expects; adjust the anchor")

io.open(path, "w", encoding="utf-8", newline="\n").write(src.replace(anchor, node + anchor))
PY
fi

grep -q 'everest,es8311' "$ov" || { echo "FAIL: the overlay node did not land" >&2; exit 1; }

echo
echo "grafted. Build it with:"
echo "  west twister -p native_sim -T tests/drivers/build_all/audio --inline-logs"
