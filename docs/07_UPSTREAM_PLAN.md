# Upstream Contribution Plan

## Principle

Small PRs beat one giant PR.

## Proposed patch split

1. **Board skeleton PR**
   - board metadata
   - defconfig
   - minimal DTS
   - documentation
   - boot/console evidence

2. **Display enablement PR**
   - ST7789P3/ST7789V node
   - sample documentation
   - display evidence

3. **BMI270 enablement PR**
   - I2C + BMI270 node
   - sensor sample evidence

4. **Power/PMIC — reuse upstream, do NOT re-invent**
   - The canonical M5PM1 driver is already being upstreamed: **PR #109961**
     (Benjamin Cabé, draft, 2026-05-27) adds an M5PM1 MFD + gpio + adc + regulator
     suite (`m5stack,m5pm1*` bindings) for the PaperColor board.
   - This repo's `m5stack,m5pm1-l3b-regulator` is an **interim out-of-tree** driver.
     For the StickS3 board PR, depend on / reuse the #109961 M5PM1 MFD driver and
     drop the interim one (align the DTS to the upstream `m5stack,m5pm1` bindings).
   - Coordinate with the #109961 author; if it stalls, offer help rather than a
     competing driver.

5. **Audio/IR experimental PRs**
   - only after verifying subsystem expectations with maintainers.

## PR description checklist

- What hardware is being added?
- Why existing board targets are insufficient?
- What was tested?
- Exact build command.
- Exact flash command.
- Serial log excerpt.
- Photos/videos when appropriate.
- Known limitations.
- Scope boundaries.

## Upstream requirements verified 2026-05-30

From the current Zephyr board-porting guide + contributor expectations:

- **Twister metadata**: ship `m5stack_sticks3_procpu.yaml` (and appcpu) listing
  supported features/RAM/flash/toolchains so CI exercises the board. (Done.)
- **Docs**: `doc/index.rst` from the board template + a board image (`.webp`);
  build docs locally.
- **Maintainership**: add an entry for `boards/m5stack/m5stack_sticks3/` to
  `MAINTAINERS.yml` — upstream expects every new board to have a maintainer.
- **Defaults**: onboard component DT nodes enabled by default; do not enable
  subsystems in board defconfig beyond what boot needs; provide `zephyr,console`.
- **Process**: DCO `Signed-off-by:` on every commit; small, bisectable commits
  (each builds); rebase (no merge/fixup commits); pass all CI/Twister before PR.
- **Boot strategy**: v0.1 ships ESP **simple boot** (single procpu image, no
  `Kconfig.sysbuild`), matching `m5stack_stamps3`. Add MCUboot + `--sysbuild` +
  `partitions_0x0_amp.dtsi` only when introducing OTA or an appcpu image.
- **PR1 scope note**: board skeleton + boot/console/buttons + BMI270 (polled);
  LCD as PR2; PSRAM enable + PMIC/audio/IR as later PRs/RFCs.

Sources: docs.zephyrproject.org/latest/hardware/porting/board_porting.html ;
docs.zephyrproject.org/latest/contribute/contributor_expectations.html
