# Contributing & project conventions

This project provides a Zephyr RTOS board port for the M5Stack StickS3
(ESP32-S3-PICO-1-N8R8). It is optimized for upstreamability, reproducibility,
honesty, and small reviewable changes. You do **not** need any AI tooling to
build, flash, or contribute — everything here uses standard Zephyr/Espressif
tooling (`west`, the Zephyr SDK, `esptool`).

## Non-negotiable rules

- Do not claim hardware works without captured evidence: a build log, flash log,
  serial log, and a photo/video, saved under `evidence/`.
- Keep the first public release small: boot, console, buttons, LCD, IMU,
  documentation. Audio/IR/full power-management are roadmap/experimental unless
  verified.
- Follow Zephyr style and contribution conventions; prefer small, self-contained
  PRs with DCO `Signed-off-by` on every commit.
- Commits carry no automated/bot authorship footers.
- Before marking a task done, run `bash verify.sh` from the repository root.
- Update `docs/02_SDD.md` and `docs/03_TDD.md` before implementation changes.
- Do not overwrite source-derived pin mappings without citing the source
  (datasheet, schematic, vendor code, or a hardware measurement).

## Verification levels

Public claims must use this gradation (see `docs/05_VALIDATION_MATRIX.md`):

- **scaffolded** — file exists, not built.
- **build-verified** — compiles under Zephyr (board discovery + `west build`).
- **flash-verified** — flashes to hardware with a verified hash.
- **runtime-verified** — observed running on hardware via serial/photo/video.
- **upstream-reviewed** — accepted/feedback from Zephyr maintainers.

## Setup, build, flash

```bash
bash verify.sh                          # repo integrity (always runnable)
bash scripts/bootstrap_zephyr_ubuntu.sh # one-time Zephyr 4.4 workspace + SDK
bash scripts/build_m5sticks3.sh         # build the validation app for the board
```

```bash
west build -p always -b m5stack_sticks3/esp32s3/procpu app
west flash            # on Linux with the device attached
west espressif monitor
```

On Windows, flash/monitor from the host (ESP32-S3 USB-Serial/JTAG needs
`esptool --after watchdog-reset`) — see `docs/10_HARDWARE_FLASHING_NOTES.md` and
`scripts/flash_windows.ps1` / `scripts/monitor_windows.ps1`.

## Code style

- C: follow nearby Zephyr style; no clever abstractions in board bring-up.
- Devicetree: tabs for indentation; dashes in node/property names; underscores
  in labels. Do not change StickS3 pins without citing a source/measurement.
  Keep experimental devices `disabled` until build/hardware verified; prefer
  existing Zephyr bindings before creating new ones.
- Markdown: short sections; copy-pasteable command blocks; source links for
  hardware claims.
- Shell: `set -euo pipefail`; scripts must be reviewable before execution.

## Testing

- Run `bash verify.sh` after edits.
- If a Zephyr workspace exists, run `bash scripts/build_m5sticks3.sh` after
  board/app changes.
- Driver logic is unit-tested without hardware:
  `west build -b native_sim tests/drivers/regulator/m5pm1 -- -DZEPHYR_EXTRA_MODULES=$PWD`.
- Save build/flash/serial logs (and photos for hardware milestones) under
  `evidence/`. Never mark a hardware test passed without logs plus photo/video.

## Branching

Use focused branches, e.g. `bringup/boot-console`, `bringup/lcd-st7789p3`,
`bringup/bmi270-imu`, `bringup/power-m5pm1`, `docs/upstream-pr-plan`.

## Definition of Done

1. Requirements updated in SDD/TDD.
2. Code or docs changed in a focused way.
3. `bash verify.sh` passes.
4. For hardware tasks, serial/photo/video evidence saved under `evidence/`.
5. Known limitations listed in `docs/06_RISK_REGISTER.md` or the relevant README.
