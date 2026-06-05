/*
 * Copyright (c) 2026 Hsiu-Chi Tsai
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "battery.h"

uint8_t bat_soc_bars(int soc, uint8_t nbars)
{
	if (soc <= 0 || nbars == 0U) {
		return 0U;
	}
	if (soc >= 100) {
		return nbars;
	}

	/* Rounded: (soc * nbars + 50) / 100. soc is 1..99 here, so the product
	 * fits an int and the result is in 0..nbars-1.
	 */
	return (uint8_t)(((soc * (int)nbars) + 50) / 100);
}

bool bat_power_present(int vin_mv)
{
	return vin_mv >= BAT_VIN_PRESENT_MV;
}
