#!/usr/bin/env python3
# Copyright (c) 2026 Hsiu-Chi Tsai
# SPDX-License-Identifier: Apache-2.0
"""Sync the upstream ES8311 driver copy into this repo.

The upstream copy targets Zephyr main, where the audio codec became a proper
driver class in zephyrproject-rtos/zephyr#110631 (after v4.4.0): `struct
audio_codec_api` is now a deprecated alias and drivers register with
`DEVICE_API(audio_codec, ...)`.

This repo pins Zephyr v4.4.0, where that class does not exist, so the copy here
must keep the pre-#110631 form. That single line is the ONLY difference between
the two files, and this script is what enforces it: it copies the upstream file
verbatim and rewrites that one declaration, leaving a comment in its place that
says so.

Usage:
    python3 scripts/sync_es8311_upstream.py <path-to-upstream-es8311.c>

Run it after every change to the upstream copy, then run the ztest.
"""
import io
import os
import sys

UPSTREAM_DECL = "static DEVICE_API(audio_codec, es8311_api) = {"

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
REPO_DRIVER = os.path.join(HERE, os.pardir, "drivers", "audio", "es8311.c")


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2

    src = io.open(argv[1], encoding="utf-8", newline="\n").read()
    if src.count(UPSTREAM_DECL) != 1:
        print("error: the upstream copy must carry exactly one DEVICE_API(audio_codec, "
              "es8311_api) declaration; found %d" % src.count(UPSTREAM_DECL))
        return 1

    out = src.replace(UPSTREAM_DECL, IN_REPO_DECL)
    io.open(REPO_DRIVER, "w", encoding="utf-8", newline="\n").write(out)

    over = [(i + 1, len(line)) for i, line in enumerate(out.split("\n")) if len(line) > 100]
    print("synced %s -> drivers/audio/es8311.c (v4.4.0 API form)" % argv[1])
    print("  lines over 100 columns: %s" % (over if over else "none"))
    return 1 if over else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
