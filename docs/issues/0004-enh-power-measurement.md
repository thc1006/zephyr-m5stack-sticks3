<!--
Suggested GitHub labels: Enhancement, area: Power Management
gh issue create --title "M5StickS3: on-device power measurement via M5PM1 ADC" \
  --body-file docs/issues/0004-enh-power-measurement.md --label "Enhancement"
-->
# Enhancement: On-device power measurement via M5PM1 ADC (no external tool)

**Is your enhancement proposal related to a problem? Please describe.**
A PMIC's value is power saving, but the current port has no way to quantify it,
and no precision power tool (e.g. Nordic PPK2) is available. We still want
real numbers for each power state for documentation and regression.

**Describe the solution you'd like**
Use the **M5PM1's own ADC / battery telemetry** as the measurement instrument
(no external hardware): expose battery voltage, current (charge +/ discharge -),
5V-in/out and the internal VREF/ADC channels via the fuel-gauge/charger driver
(depends on issue 0001), then log per-state values (active, each L0-L3B level,
deep sleep) and wake latency.

**Describe alternatives you've considered**
- Nordic PPK2 / Joulescope (most accurate; not available).
- USB-C inline power meter (coarse, whole-board).
- INA219/INA226 shunt sensor (mA-range, poor at uA).
The PMIC's own ADC needs no extra hardware and gives real on-device numbers.

**Additional context**
- Produces a publishable "per-power-level current + wake latency" dataset.
- Honest limit: the PMIC ADC is not lab-grade; document accuracy.

**Acceptance criteria**
- Battery V/I and per-state current readable on-device and logged to `evidence/`.
