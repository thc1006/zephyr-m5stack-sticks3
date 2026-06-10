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
     (Benjamin Cabé, MERGED 2026-06-03) adds an M5PM1 MFD + gpio + adc + regulator
     suite (`m5stack,m5pm1*` bindings) for the PaperColor board.
   - This repo **vendors** the #109961 MFD/ADC/GPIO drivers + bindings
     (`drivers/{mfd,adc,gpio}/*_m5pm1.c`, `dts/bindings/{mfd,adc,gpio}/`) as an
     interim copy, with the source commit recorded in each file header. **Delete
     them on the next Zephyr bump past 4.4.0** (#109961 merged 2026-06-03) and depend
     on the upstream module. The StickS3 board gates
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
   - **Battery SoC% (issue #8) is repo-local until a Zephyr bump** (#109961 merged
     2026-06-03): the `vbatt`
     (`voltage-divider`) and `fuel_gauge` (`zephyr,fuel-gauge-composite`)
     consumer nodes bind against the vendored M5PM1 ADC, so they cannot ship in
     the upstream board DTS until the MFD/ADC bindings merge. Both are stock
     upstream bindings (no new driver), so once #109961 is in, this is a small
     follow-up board patch (or a board sample overlay) that adds the two nodes on
     top of the upstream M5PM1 ADC. The SoC is voltage-only OCV (no coulomb
     counter), so the upstream value is an approximate gauge, consistent with the
     silicon limit above.

5. **ES8311 audio codec (issue #7) — engage upstream, do NOT open a competing PR**
   - The in-repo `drivers/audio/es8311.c` is a standalone, **board-independent**
     driver against the Zephyr audio codec API: playback HW-verified on the StickS3
     (HW-006, 440 Hz beep), an ADC/capture route added and HW-verified (HW-016d),
     native_sim ztest **11/11** (includes `test_configure_capture_sequence` /
     `test_configure_capture_only`).
   - **Superseded framing**: the original "propose it as its own PR" plan is
     replaced by **ADR 0004** (`docs/adr/0004-es8311-upstream-engage-pr107660.md`).
     Upstream already has a live ES8311 effort (PR #107660), so opening a competing
     driver would duplicate/fragment it. Decision: **engage #107660**, and
     contribute our differentiator — the HW-verified **capture/ADC route**, which
     #107660 omits — as a clean follow-up once the playback codec lands.
   - **Current action (per ADR 0004 Update): HOLD.** Engage only after (a) the base
     board PR #107655 lands and (b) the ES8311 work resumes on a live PR. Until
     then this driver stays repo-local. See ADR 0004 for the trigger conditions and
     the parked, trimmed engagement comment.

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

7. **Wi-Fi (station) — enable `&wifi` on the in-tree board**
   - The esp32 wifi driver is already upstream, so the contribution is
     board-level: enable the `&wifi` node on `m5stack_sticks3` and document Wi-Fi
     under `zephyr:board-supported-hw`, backed by the captured scan + connect
     evidence (HW-014/HW-015). The radio is the SoC's; nothing board-specific
     beyond turning the node on.
   - Keep it a focused follow-up after the board skeleton PR (#110205) lands,
     consistent with the LCD/PMIC/audio/IR roadmap. WPA2-PSK only (WPA3-SAE does
     not build against the current tree); SPIRAM stays off for Wi-Fi.
   - Do NOT upstream the demo's build-time auto-connect / credentials path. For a
     board Wi-Fi sample the idiom is runtime credentials (the existing
     `samples/net/wifi/shell` or `net config`), so the binary carries no secret
     and one image works against any AP. Our `CONFIG_APP_WIFI_SSID/PSK`
     auto-connect is a local demo convenience only.

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
