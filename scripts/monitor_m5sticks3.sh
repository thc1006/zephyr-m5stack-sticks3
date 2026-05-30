#!/usr/bin/env bash
set -euo pipefail

WORKSPACE_DIR="${WORKSPACE_DIR:-$HOME/zephyrproject-m5sticks3}"
cd "$WORKSPACE_DIR"
# shellcheck disable=SC1091
source .venv/bin/activate
west espressif monitor
