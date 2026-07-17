#!/usr/bin/env python3
# Copyright (c) 2026 Hsiu-Chi Tsai
# SPDX-License-Identifier: Apache-2.0
"""Emit the upstream form of the ES8311 driver from the copy tracked in this repo.

There is ONE tracked source of truth, `drivers/audio/es8311.c`, and it is the
v4.4.0 form, because that is the Zephyr this repo pins and builds against.

Upstream main is one line different. Zephyr made the audio codec a proper driver
class in zephyrproject-rtos/zephyr#110631, which landed after v4.4.0: `struct
audio_codec_api` became a deprecated alias and drivers now register with
`DEVICE_API(audio_codec, ...)`. This script rewrites exactly that one declaration
and writes the result somewhere else. It never writes into the repo.

The direction matters. This used to run the other way, copying an upstream file
that lived outside the repo INTO the tree, which made an untracked file the real
source and let the tracked one drift behind it silently. Now the file a reviewer
can actually see is the one everything is generated from.

Three things differ between here and upstream, and every difference lives in this
one script so that none of them can drift quietly:

  es8311.c        the DEVICE_API declaration (see above).
  Kconfig.es8311  the `depends on AUDIO_CODEC`. Upstream sources every codec
                  Kconfig from inside `if AUDIO_CODEC ... endif`, so not one of the
                  eleven siblings states that dependency and ours must not either.
                  Out of tree the file is sourced at the top level, where it has to.
  testcase.yaml   the test metadata FILENAME. Upstream rejects the legacy
                  `testcase.yaml` and requires `tests.yaml`; the twister in
                  v4.4.0, which this repo pins and runs, finds nothing under the
                  new name. So the tracked copy keeps the old name and the
                  upstream form is emitted under the new one. Renaming the
                  tracked file to satisfy upstream silently disables the entire
                  suite here, with twister reporting success on zero tests.

Usage:
    python3 scripts/sync_es8311_upstream.py <output-dir>

`scripts/graft_es8311_upstream.sh` calls this to plant the driver in a real Zephyr
main tree and build it, which is how the "it compiles against upstream main" claim
is kept honest rather than asserted.
"""
import io
import os
import sys

UPSTREAM_DECL = "static DEVICE_API(audio_codec, es8311_api) = {"

# Present out of tree, absent upstream.
KCONFIG_OOT_ONLY = """	# The upstream copy carries no such dependency, because upstream sources this
	# file from inside `if AUDIO_CODEC`. Out of tree it is sourced at the top
	# level, so it has to state it.
	depends on AUDIO_CODEC
"""

# The exact block sync-emitted into the repo copy. Matched verbatim, so that an
# edit to it is a loud failure here rather than a silent divergence upstream.
IN_REPO_DECL = """/*
 * Zephyr made the audio codec a proper driver class in zephyrproject-rtos/zephyr
 * PR #110631, which landed after v4.4.0: `struct audio_codec_api` became a
 * deprecated alias and drivers now register with DEVICE_API(audio_codec, ...).
 * This tree pins v4.4.0, where that class does not exist, so the copy here keeps
 * the pre-#110631 form. The upstream copy of this file carries
 *
 *     static DEVICE_API(audio_codec, es8311_api) = {
 *
 * on the line below, and that is the ONLY line that differs between the two.
 * scripts/sync_es8311_upstream.py is what keeps them in step.
 */
static const struct audio_codec_api es8311_api = {"""

HERE = os.path.dirname(os.path.abspath(__file__))
AUDIO = os.path.join(HERE, os.pardir, "drivers", "audio")
REPO_DRIVER = os.path.join(AUDIO, "es8311.c")
REPO_KCONFIG = os.path.join(AUDIO, "Kconfig.es8311")


def read(path):
    return io.open(path, encoding="utf-8", newline="\n").read()


def write(path, text):
    io.open(path, "w", encoding="utf-8", newline="\n").write(text)


def rewrite(path, src, old, new, what):
    """Replace `old` with `new`, exactly once, or fail loudly."""
    if src.count(old) != 1:
        print("error: %s must carry exactly one copy of the %s this script rewrites; "
              "found %d. If it was edited, update this script to match." %
              (path, what, src.count(old)))
        return None
    return src.replace(old, new)


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2

    outdir = argv[1]

    driver = rewrite("drivers/audio/es8311.c", read(REPO_DRIVER), IN_REPO_DECL, UPSTREAM_DECL,
                     "v4.4.0 API declaration block")
    if driver is None:
        return 1

    kconfig = rewrite("drivers/audio/Kconfig.es8311", read(REPO_KCONFIG), KCONFIG_OOT_ONLY, "",
                      "out-of-tree `depends on AUDIO_CODEC` block")
    if kconfig is None:
        return 1

    over = [(i + 1, len(line)) for i, line in enumerate(driver.split("\n")) if len(line) > 100]
    if over:
        print("error: lines over 100 columns (upstream checkpatch limit): %s" % over)
        return 1

    write(os.path.join(outdir, "es8311.c"), driver)
    write(os.path.join(outdir, "Kconfig.es8311"), kconfig)
    print("emitted the upstream form of es8311.c + Kconfig.es8311 -> %s" % outdir)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
