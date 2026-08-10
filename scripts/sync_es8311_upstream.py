#!/usr/bin/env python3
# Copyright (c) 2026 Hsiu-Chi Tsai
# SPDX-License-Identifier: Apache-2.0
"""Emit the upstream form of the ES8311 driver from the copy tracked in this repo.

There is ONE tracked source of truth, `drivers/audio/es8311.c`, and it is the
v4.4.0 form, because that is the Zephyr this repo pins and builds against.

The driver itself is now byte-identical to the upstream copy: this tree tracks a
Zephyr that has the audio-codec driver class from zephyrproject-rtos/zephyr#110631,
so both sides use `DEVICE_API(audio_codec, ...)`. What still differs is the Kconfig,
which has to state `depends on AUDIO_CODEC` out of tree because upstream sources the
same file from inside `if AUDIO_CODEC`. This script emits the upstream form
somewhere else. It never writes into the repo.

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

# Present out of tree, absent upstream.
KCONFIG_OOT_ONLY = """	# The upstream copy carries no such dependency, because upstream sources this
	# file from inside `if AUDIO_CODEC`. Out of tree it is sourced at the top
	# level, so it has to state it.
	depends on AUDIO_CODEC
"""

HERE = os.path.dirname(os.path.abspath(__file__))
AUDIO = os.path.join(HERE, os.pardir, "drivers", "audio")
REPO_DRIVER = os.path.join(AUDIO, "es8311.c")
REPO_KCONFIG = os.path.join(AUDIO, "Kconfig.es8311")

# Files that go upstream byte-for-byte. They need no rewriting, but they DO need
# to travel with the driver: on 2026-08-09 the driver was synced with a
# rate-dependent DAC_OSR while the test upstream still asserted the old constant,
# and twister failed on a driver disagreeing with its own test. Emitting them here
# makes that a mechanical step rather than something to remember.
VERBATIM = [
    os.path.join(AUDIO, "emul_es8311.c"),
    os.path.join(AUDIO, "emul_es8311.h"),
    os.path.join(HERE, os.pardir, "tests", "drivers", "audio", "es8311", "src", "main.c"),
]


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

    driver = read(REPO_DRIVER)

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
    names = ["es8311.c", "Kconfig.es8311"]

    for src in VERBATIM:
        if not os.path.exists(src):
            print("error: %s is missing; it must travel with the driver" % src)
            return 1
        name = os.path.basename(src)
        write(os.path.join(outdir, name), read(src))
        names.append(name)

    print("emitted -> %s: %s" % (outdir, ", ".join(names)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
