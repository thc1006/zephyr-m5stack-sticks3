#!/usr/bin/env bash
# Copyright (c) 2026 Hsiu-Chi Tsai
# SPDX-License-Identifier: Apache-2.0
#
# Build the FULL comprehensive demo for the M5StickS3: the default app plus the
# optional features - BLE telemetry (overlay-ble.conf, CONFIG_APP_BLE), ES8311
# audio (overlay-audio.conf, CONFIG_APP_AUDIO), and IR NEC (overlay-ir.conf,
# CONFIG_APP_IR; TX runtime-verified, RX experimental). IR uses no blob.
#
# NOTE: BLE pulls in the Espressif Bluetooth controller HAL blob, so this build
# needs the blob fetched once first:
#
#   west blobs fetch hal_espressif
#
# The default build (scripts/build_m5sticks3.sh) deliberately omits both
# overlays so it stays blob-free and CI-green; only this demo build requires the
# blob. Audio adds the codec/I2S/DMA stack but no blob of its own.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKSPACE_DIR="${WORKSPACE_DIR:-$HOME/zephyrproject-m5sticks3}"

if [ ! -d "$WORKSPACE_DIR/zephyr" ]; then
  echo "Zephyr workspace not found at $WORKSPACE_DIR/zephyr" >&2
  echo "Run: bash scripts/bootstrap_zephyr_ubuntu.sh" >&2
  exit 1
fi

# shellcheck disable=SC1091
source "$WORKSPACE_DIR/.venv/bin/activate"
cd "$WORKSPACE_DIR"
export BOARD_ROOT="$REPO_ROOT"
# Build the repo in as an out-of-tree module so its drivers/ + dts/bindings/
# (the M5PM1 MFD/ADC/GPIO + the ES8311 codec) are compiled.
export ZEPHYR_EXTRA_MODULES="$REPO_ROOT"
BOARD="${BOARD:-m5stack_sticks3/esp32s3/procpu}"
west build -p always -b "$BOARD" "$REPO_ROOT/app" \
  -- -DEXTRA_CONF_FILE="overlay-ble.conf;overlay-audio.conf;overlay-ir.conf"
