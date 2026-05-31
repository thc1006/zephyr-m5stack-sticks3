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
   - This repo **vendors** the #109961 MFD/ADC/GPIO drivers + bindings
     (`drivers/{mfd,adc,gpio}/*_m5pm1.c`, `dts/bindings/{mfd,adc,gpio}/`) as an
     interim copy, with the source commit recorded in each file header. **Delete
     them on merge** and depend on the upstream module. The StickS3 board gates
     the L3B/LCD rail with a stock `regulator-fixed` on the MFD gpio child, so no
     M5PM1-specific regulator is needed (the earlier interim
     `m5stack,m5pm1-l3b-regulator` driver + its ztest have been removed).
   - **Local delta worth raising on #109961**: `mfd_m5pm1.c` adds an idle-sleep
     disable (reg 0x09 = 0x00) and a wake-retry on the first I2C transfer, needed
     for the StickS3's M5PM1 to respond reliably at boot. Offer this upstream
     (issue/comment on #109961) rather than carrying it forever out-of-tree.
   - Coordinate with the #109961 author; if it stalls, offer help rather than a
     competing driver.
   - **Silicon limit (verified 2026-06-01)**: the M5PM1 has no battery-current,
     charge-current, coulomb-counter or SoC register (voltages only). Any "full
     PMIC" upstream work is limited to charge-enable + power-source/insertion
     status; there is no fuel-gauge to contribute, and the current/per-state
     dataset (issue #4) is not obtainable on-device.

5. **ES8311 audio codec PR (task #21)**
   - The in-repo `drivers/audio/es8311.c` is a standalone driver against the
     Zephyr audio codec API (native_sim ztest 9/9). It is **board-independent**
     and is the natural first audio upstream contribution — propose it as its own
     PR (codec driver + `everest,es8311` binding + the ztest), separate from the
     board port. Hardware-validated on the StickS3 (HW-006).

6. **IR (NEC) — in-repo on stock PWM drivers; an RMT driver is a separate big PR**
   - Zephyr 4.4 has no ESP32 RMT driver and no consumer-IR subsystem (verified
     2026-06-01). The StickS3 IR feature is built in-app on stock LEDC (TX, G46) +
     a GPIO edge interrupt (RX, G42) with NEC encode/decode, gated `CONFIG_APP_IR`.
     Both TX and RX are HW-verified (TX emits NEC; the G42 receiver gets ~10k
     edges from a real remote; NEC decodes via on-device loopback). No new
     low-level driver, so nothing board-specific to upstream beyond the board DTS.
   - Two genuinely-upstream-worthy but separate, larger efforts (out of scope
     here): a proper Zephyr **ESP32 RMT driver** (binding + driver + DMA - the
     right peripheral for IR, with a HW FIFO that avoids the MCPWM edge-drop), and
     a **Zephyr consumer-IR subsystem** to host protocol decoders (NEC/RC5/RC6/
     SIRC/...). Without that subsystem, protocol decoders have no upstream home,
     so the in-repo app supports NEC only. Raise both with maintainers (RFC) first.

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
