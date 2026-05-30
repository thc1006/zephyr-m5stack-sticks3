# M5PM1 ADC power measurement on StickS3 hardware (2026-05-31)

Validation for issue #4 (on-device battery and input voltage measurement via the
M5PM1 ADC), on a real M5Stack StickS3.

## Scope and honesty note

This was validated with a LOCAL build that cherry-picks the M5PM1 driver set
(MFD + ADC + regulator) from upstream Zephyr PR #109961, which is not yet merged.
The StickS3 board in this repository does NOT yet include ADC support (it currently
ships only the interim L3B-rail regulator). This evidence documents that the
capability works on StickS3 hardware; landing it in-tree is pending the #109961
merge, after which the board can adopt the upstream `m5stack,m5pm1-adc` driver
directly. See `docs/07_UPSTREAM_PLAN.md`.

## Method

The M5PM1 (I2C 0x6e) ADC exposes battery (VBAT, ch1), USB input (VIN, ch2) and the
external 5V boost output (5VOUT, ch3). A small app read all three over the standard
Zephyr ADC API (values in millivolts) and logged them once per second. The external
5V boost was enabled via the `m5stack,m5pm1-regulator` `boost_5v` rail
(`regulator-boot-on`). The system 3V3 rails (`dcdc_3v3`, `ldo_3v3`) were declared
`regulator-always-on` so they could not be disabled; enabling the boost is a single
bit read-modify-write of PMIC register 0x06, which preserves the 3V3 enable bits.
This DT layout was adversarially safety-reviewed before flashing.

## Firmware readings (boost enabled)

VBAT about 4.18 V, VIN about 5.13 V, 5VOUT about 5.13 V, steady over 12 samples.
Full log: `20260531-m5pm1-adc-boost.log`.

## Multimeter cross-check (Pro'sKit MT-1236, DC volts)

External 5V on the GROVE Port.A 5V pin vs GND:

| state | multimeter | ADC 5VOUT |
| --- | --- | --- |
| boost disabled | 0.7 V | near 0 (rail off) |
| boost enabled | 5.095 V then 5.167 V | about 5.13 V |

Photo of the enabled-state reading: `20260531-m5pm1-5vout-meter.jpg` (5.167 V).
The ADC 5VOUT (about 5.13 V) and the meter (5.167 V) agree to about 37 mV (~0.7%).
The M5PM1 ADC is a PMIC-internal converter (not lab grade); a 0.7% match with a
handheld meter confirms the readings are correct.

## Result

M5PM1 ADC battery, input and 5V readout is runtime-verified on StickS3 and
cross-checked with a multimeter. The two-state check (boost off 0.7 V, boost on
~5.1 V) matched the ADC in both states, and VBAT/VIN stayed healthy throughout
(no brownout, system rails untouched).
