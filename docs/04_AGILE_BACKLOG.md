# Agile Backlog

## Sprint 0 — repository and research baseline

**Goal:** establish reproducible, honest project conventions.

- [x] Create `CONTRIBUTING.md` (mission, rules, verification levels, DoD).
- [x] Create SDD/TDD/ADR/backlog.
- [x] Add verification script.
- [x] Re-run prior-art search immediately before public GitHub release
  (2026-05-30, 3 independent passes; first-public-Zephyr-board-port claim holds,
  see `docs/00_RESEARCH_SNAPSHOT_UPDATED.md`; M5PM1 driver = reuse PR #109961).

## Sprint 1 — board target discoverability

**User story:** As a Zephyr developer, I can run `west build -b m5stack_sticks3 app` and get a meaningful build result.

Tasks:

- [x] Compare board file layout with nearby upstream M5Stack ESP32-S3 boards.
- [x] Fix board metadata for Zephyr 4.4 hardware model (HWMv2 procpu/appcpu).
- [x] Verify defconfig and DTS include paths.
- [x] Save build log (`evidence/20260530-0345-build.log`).

Acceptance criteria:

- `west boards | grep m5stack_sticks3` works, or equivalent board discovery is documented.
- Validation app reaches compile stage.

## Sprint 2 — boot and console

- [x] Flash validation app (esptool, simple boot @ 0x0).
- [x] Capture serial boot log (`evidence/20260530-0432-serial.log`).
- [x] Document download-mode steps (`docs/10_HARDWARE_FLASHING_NOTES.md` — `--after watchdog-reset`).

## Sprint 3 — buttons

- [x] Enable GPIO keys for G11/G12 (active-low + pull-up).
- [x] Add button readout to the validation app.
- [x] Validate press (G11/G12 pull low; `evidence/20260530-buttons.log`).

## Sprint 4 — LCD ST7789P3 (DONE)

Hardware finding 2026-05-30: the LCD/audio/IR rail is gated by the M5PM1 PMIC
(PYG2 / L3B). Wrote a new `m5stack,m5pm1-l3b-regulator` driver to enable it, then
brought up the panel.

- [x] Enable M5PM1 L3B rail via new PMIC regulator driver (PYG2 high).
- [x] Use `zephyr,mipi-dbi-spi` + `sitronix,st7789v` on SPI2, panel on CS0.
- [x] Bring up backlight G38 (regulator-fixed).
- [x] x/y offsets = 52/40 (from M5GFX), verified on hardware.
- [x] Color-fill test; panel cycles R/G/B/W (`evidence/20260530-lcd.log`).

## Sprint 5 — BMI270 IMU

- [x] Enable I2C pins G48/G47.
- [x] Add BMI270 at 0x68.
- [x] Validate movement readings (gravity tracks X/Y/Z; `evidence/20260530-imu.log`).

## Sprint 6 — M5PM1 power

- [ ] Research protocol from official M5PM1 docs/library.
- [ ] Decide whether existing Zephyr regulator model fits.
- [ ] Implement minimal EXT_5V control or document blocker.

## Sprint 7 — IR and audio experiments

- [ ] Confirm ESP32 RMT support state in local Zephyr tree.
- [ ] Create IR TX proof-of-concept.
- [ ] Research ES8311 support status.
- [ ] Create audio register-read proof-of-concept.

## Sprint 8 — upstream preparation

- [ ] Split PRs into small reviewable changes.
- [ ] Create board documentation.
- [ ] Attach validation logs.
- [ ] Request maintainer feedback early as draft PR if needed.
