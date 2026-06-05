/*
 * Copyright (c) 2026 Hsiu-Chi Tsai
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef M5STICKS3_BATTERY_H
#define M5STICKS3_BATTERY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Pure battery-display helpers (no Zephyr headers, native_sim-testable). The
 * state-of-charge percent itself comes from the upstream
 * zephyr,fuel-gauge-composite OCV lookup (issue #8); these only map it to a UI
 * bar and classify the power source from the VIN reading. Mirrors the nec.c /
 * audio_dsp.c pure-logic split.
 */

/*
 * VIN (mV) at or above this means external 5V / USB power is present. On battery
 * the M5PM1 VIN reads far below this; with USB attached it reads ~5000 mV. This
 * is a coarse "ext power present" signal, NOT a charge-state read (the M5PM1 has
 * no reliable charging-status register).
 */
#define BAT_VIN_PRESENT_MV 4000

/*
 * Map a state-of-charge percent (0..100) to a 0..nbars bar count, rounded and
 * clamped. A negative soc (read error) or nbars == 0 returns 0; soc >= 100
 * returns nbars.
 */
uint8_t bat_soc_bars(int soc, uint8_t nbars);

/*
 * True if external power (USB / 5V) is present, from the VIN reading in mV. A
 * negative vin_mv (read error / channel unavailable) returns false.
 */
bool bat_power_present(int vin_mv);

#endif /* M5STICKS3_BATTERY_H */
