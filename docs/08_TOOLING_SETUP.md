# Tooling Setup

## Host assumption

Recommended: Ubuntu 24.04 or WSL2 Ubuntu 24.04. Linux is preferred because Zephyr Twister execution support is most complete on Linux.

## Bootstrap

Review before running:

```bash
bash scripts/bootstrap_zephyr_ubuntu.sh
```

This script pins Zephyr to `v4.4.0` for reproducibility, because Zephyr 4.4.0 is the latest stable release in the 2026-05-30 research snapshot.

## Git workflow

```bash
git init
git add .
git commit -s -m "docs: scaffold M5StickS3 Zephyr enablement kit"
```

Sign off every commit with DCO (`git commit -s`). No automated/bot co-author footers.
