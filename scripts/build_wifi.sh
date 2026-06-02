#!/usr/bin/env bash
# Copyright (c) 2026 Hsiu-Chi Tsai
# SPDX-License-Identifier: Apache-2.0
#
# Build the M5StickS3 app with the Wi-Fi station feature (overlay-wifi.conf,
# CONFIG_APP_WIFI). This pulls in the ESP32-S3 native Wi-Fi driver, the
# networking stack, and the driver's bundled (ESP-IDF) supplicant, so it needs
# the Espressif HAL blob fetched once first:
#
#   west blobs fetch hal_espressif
#
# Wi-Fi is mutually exclusive with the BLE overlay (Wi-Fi + BLE coexistence is
# not a validated Zephyr configuration on the ESP32-S3), so this build does not
# include overlay-ble.conf. overlay-wifi.overlay enables the &wifi devicetree
# node, which the board DTS keeps disabled until the feature is runtime-verified.

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
# (the M5PM1 MFD/ADC/GPIO regulator the board's LCD rail needs) are compiled.
export ZEPHYR_EXTRA_MODULES="$REPO_ROOT"
BOARD="${BOARD:-m5stack_sticks3/esp32s3/procpu}"

# Include the local, untracked Wi-Fi credentials overlay if present, so a connect
# (CONFIG_APP_WIFI_SSID/PSK) can be runtime-tested without committing secrets.
EXTRA_CONF="overlay-wifi.conf"
if [ -f "$REPO_ROOT/app/wifi-creds.local.conf" ]; then
  EXTRA_CONF="$EXTRA_CONF;wifi-creds.local.conf"
  echo "build_wifi.sh: including local Wi-Fi credentials overlay (untracked)."
fi

west build -p always -b "$BOARD" "$REPO_ROOT/app" \
  -- -DEXTRA_CONF_FILE="$EXTRA_CONF" \
     -DEXTRA_DTC_OVERLAY_FILE="overlay-wifi.overlay"
