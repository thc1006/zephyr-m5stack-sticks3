#!/usr/bin/env bash
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
# (e.g. the M5PM1 PMIC regulator) are compiled.
export ZEPHYR_EXTRA_MODULES="$REPO_ROOT"
BOARD="${BOARD:-m5stack_sticks3/esp32s3/procpu}"
west build -p always -b "$BOARD" "$REPO_ROOT/app"
