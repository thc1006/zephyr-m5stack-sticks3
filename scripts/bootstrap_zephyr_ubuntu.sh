#!/usr/bin/env bash
set -euo pipefail

# Bootstrap a Zephyr 4.4.0 workspace next to this repository.
# Review before running. Assumes Ubuntu/WSL Ubuntu.

sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  git cmake ninja-build gperf ccache dfu-util device-tree-compiler wget \
  python3-dev python3-pip python3-venv python3-setuptools python3-tk \
  xz-utils file make gcc gcc-multilib g++-multilib libsdl2-dev libmagic1 \
  protobuf-compiler curl ca-certificates

WORKSPACE_DIR="${WORKSPACE_DIR:-$HOME/zephyrproject-m5sticks3}"
ZEPHYR_TAG="${ZEPHYR_TAG:-v4.4.0}"

mkdir -p "$WORKSPACE_DIR"
cd "$WORKSPACE_DIR"

python3 -m venv .venv
# shellcheck disable=SC1091
source .venv/bin/activate
pip install --upgrade pip wheel west

if [ ! -d .west ]; then
  west init -m https://github.com/zephyrproject-rtos/zephyr --mr "$ZEPHYR_TAG" .
fi

west update
west zephyr-export
pip install -r zephyr/scripts/requirements.txt
# Install per-module Python deps (e.g. esptool for the ESP32 image/flash steps).
west packages pip --install

cat <<'EOF'

Next steps:
1. Install the Zephyr SDK version recommended by your checked-out Zephyr tree.
   For Zephyr 4.4, upstream release notes identify Zephyr SDK 1.0.x as the supported line.
2. Return to this repository and run:
   WORKSPACE_DIR=$HOME/zephyrproject-m5sticks3 bash scripts/build_m5sticks3.sh
EOF
