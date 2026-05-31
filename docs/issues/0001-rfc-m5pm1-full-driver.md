<!--
Suggested GitHub labels: RFC, Enhancement, area: Power Management, platform: ESP32
File with: gh issue create --title "RFC: Full M5Stack M5PM1 PMIC support" \
  --body-file docs/issues/0001-rfc-m5pm1-full-driver.md --label "RFC,Enhancement"
-->
# RFC: Full M5Stack M5PM1 PMIC support (MFD + regulator + GPIO + charger + fuel-gauge)

## Introduction

The M5Stack StickS3 board port ships a **partial** M5PM1 driver set: an MFD
parent (`m5stack,m5pm1`) with GPIO and ADC children (vendored from Zephyr PR
#109961), gating the LCD/L3B rail (PYG2) via a `regulator-fixed` and reading
VBAT. (This superseded the earlier minimal single-rail
`m5stack,m5pm1-l3b-regulator` driver, now removed.) The M5PM1 is a full PMIC;
this RFC proposes the complete, idiomatic Zephyr support (charger, fuel-gauge,
the remaining GPIOs).

## Problem description

The minimal regulator models a single GPIO-switched rail. The chip also provides
battery charging, fuel-gauge/ADC telemetry, 5 multi-function GPIOs (PYG0-PYG4),
a 5V boost rail, watchdog, RTC RAM, NeoPixel and a wake/timer engine. None of
these are exposed, so StickS3 power management, charging, the speaker-amp enable
(PYG3) and the BMI270 interrupt (PYG4) cannot be used.

## Proposed change

Refactor into the conventional Zephyr multi-driver PMIC layout (cf. AXP192/
AXP2101, AW9523B):

- `drivers/mfd/` — `m5stack,m5pm1` I2C MFD parent (device-id check, I2C-CFG
  idle-sleep handling, shared register lock).
- `drivers/gpio/` — `m5stack,m5pm1-gpio` GPIO controller for PYG0-PYG4. The LCD
  rail then becomes a standard `regulator-fixed` with `enable-gpios`.
- `drivers/charger/` — `m5stack,m5pm1-charger` via the Zephyr charger API
  (`charger_get_prop`/`set_prop`): charge current/voltage limits, status, TS.
- `drivers/fuel_gauge/` (or sensor) — battery V/I/SOC via the ADC registers.
- `drivers/regulator/` — keep/rework rail control on top of the GPIO controller.

## Detailed RFC

- Register map and reset defaults are documented (datasheet + M5GFX source,
  see `docs/09_SOURCES.md`). I2C 0x6E; DEVICE_ID@0x00 = 0x50.
- Init priority: MFD < GPIO < regulator < consumers (display).
- Shared register access must be serialized at the MFD level once multiple
  function drivers exist (the current single regulator relies on the regulator
  core's per-device mutex; that is insufficient cross-driver).
- Li-ion safety (see issue: charger): do NOT change charge-voltage/TS defaults
  without bench validation.

## Dependencies

Blocks full power-management, audio (PYG3 amp enable), and BMI270 interrupts
(PYG4). Should land before deep-sleep / low-power work.

## Concerns and unresolved questions

- Single-rail regulator vs full MFD modeling — maintainer guidance wanted.
- Whether charger belongs in `charger` subsystem vs a custom binding.

## Acceptance criteria

- MFD + GPIO controller + at least one regulator-fixed rail build and pass an
  emulator-backed ztest on `native_sim`.
- Hardware: rails toggle, charge status reads back, battery V/I sane.

## References

- `docs/00_RESEARCH_SNAPSHOT_UPDATED.md`, `docs/09_SOURCES.md`
- https://github.com/m5stack/M5PM1 (datasheet + register map)
- Zephyr AXP2101 driver set (reference pattern)
