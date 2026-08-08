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
   - This repo **used to vendor** the #109961 MFD/ADC/GPIO drivers + bindings as
     an interim copy. **They were dropped on 2026-08-09**: the build tree now
     carries #109961 and the upstream suite is a strict superset of what was
     vendored -- the same `m5stack,m5pm1{,-adc,-gpio}` compatibles, the same
     `zephyr/drivers/mfd/m5pm1.h` (four functions we call, four more we do not),
     the same `MFD_M5PM1`/`ADC_M5PM1`/`GPIO_M5PM1` symbol names, plus a regulator
     and a watchdog this repo never had. Keeping the copy would have defined
     `MFD_M5PM1` twice in one Kconfig tree. The StickS3 board gates the L3B/LCD
     rail with a stock `regulator-fixed` on the MFD gpio child, so no
     M5PM1-specific regulator is needed (the earlier interim
     `m5stack,m5pm1-l3b-regulator` driver + its ztest have been removed).
   - **The one file that stays is `drivers/mfd/emul_m5pm1.c`**, because upstream
     ships no M5PM1 emulator and `tests/drivers/m5pm1_mfd` needs one. Its Kconfig
     now declares only `EMUL_M5PM1_MFD` and `depends on MFD_M5PM1` rather than
     defining that symbol itself.
   - **The local delta is already upstream, and better.** The vendored
     `mfd_m5pm1.c` disabled idle-sleep by writing reg 0x09 = 0x00 and retried the
     first I2C transfer, which the StickS3's M5PM1 needs to answer reliably at
     boot. Upstream does the same thing generically: an
     `idle-sleep-timeout-seconds` DT property (default 0 = disabled), a
     read-modify-write that clears `I2C_CFG[3:0]` while preserving bits 7:4
     instead of clobbering the register, and a `wake_if_needed()` before *every*
     transfer rather than only the first. Nothing needs raising on #109961.
     Dropping the copy caught one over-specified assertion in
     `tests/drivers/m5pm1_mfd`, which asserted the whole byte was 0x00 and so was
     testing the vendored clobber rather than idle-sleep being off; it now
     asserts the `SLP_TO` field. Suite is 4/4 on `native_sim` against the
     upstream driver.
   - **Silicon limit (verified 2026-06-01)**: the M5PM1 has no battery-current,
     charge-current, coulomb-counter or SoC register (voltages only). Any "full
     PMIC" upstream work is limited to charge-enable + power-source/insertion
     status; there is no fuel-gauge to contribute, and the current/per-state
     dataset (issue #4) is not obtainable on-device.
   - **Battery SoC% (issue #8) is now unblocked upstream.** The `vbatt`
     (`voltage-divider`) and `fuel_gauge` (`zephyr,fuel-gauge-composite`) consumer
     nodes used to bind against the vendored M5PM1 ADC, which is why they could
     not ship in the upstream board DTS; since 2026-08-09 they bind against the
     upstream one. Both are stock upstream bindings (no new driver), so this is a
     small follow-up board patch (or a board sample overlay) adding the two nodes
     on top of the upstream M5PM1 ADC. The SoC is voltage-only OCV (no coulomb
     counter), so the upstream value is an approximate gauge, consistent with the
     silicon limit above.

5. **ES8311 audio codec (issue #7) — GO: submit our own clean, split PR**
   - The in-repo `drivers/audio/es8311.c` is a standalone, **board-independent**
     driver against the Zephyr audio codec API: playback HW-verified on the StickS3
     (HW-006, 440 Hz beep), an ADC/capture route added and HW-verified (HW-016d),
     native_sim ztest **11/11** (includes `test_configure_capture_sequence` /
     `test_configure_capture_only`).
   - **The hold is over (2026-07-11).** ADR 0004 said "do not compete, engage the
     live upstream effort (#107660)". That effort is gone: the base board #107655
     merged on 2026-06-11, and the next day #107660 (06:55Z) and #108078 (06:58Z)
     were closed without merging; #108073 and #107661 were already closed. Every
     peripheral PR is closed and none merged. Upstream `main` now ships an
     ESP32-S3-BOX-3 whose own doc lists
     "Speaker with ES8311 audio codec", while a tree-wide code search for `es8311`
     returns exactly one hit: that sentence. No driver, no binding, and nobody
     driving one. ADR 0004's own fallback ("a fresh clean-split PR") applies.
   - **Current action: GO.** Submit the codec driver + the `everest,es8311` binding
     + the ztest as one focused PR (no board, no sample), per
     `docs/issues/0007-es8311-upstream-readiness.md`: genericize the M5Stack/HW-ID
     references in comments, DCO sign-off, checkpatch + `check_compliance.py`. Lead
     with the **capture/ADC route** — HW-verified on real silicon, and #107660 never
     had it. Re-check the upstream state any time with
     `bash scripts/check_es8311_upstream_gate.sh`.

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
