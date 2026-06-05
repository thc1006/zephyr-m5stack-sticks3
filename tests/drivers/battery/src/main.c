/*
 * Copyright (c) 2026 Hsiu-Chi Tsai
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>

#include "battery.h"

ZTEST(battery, test_soc_bars_edges)
{
	/* Empty, read-error, and zero-bars all map to 0. */
	zassert_equal(bat_soc_bars(0, 4), 0U, "0%% -> 0 bars");
	zassert_equal(bat_soc_bars(-1, 4), 0U, "read error -> 0 bars");
	zassert_equal(bat_soc_bars(-100, 4), 0U, "negative -> 0 bars");
	zassert_equal(bat_soc_bars(50, 0), 0U, "nbars 0 -> 0 bars");

	/* Full and over-100 clamp to nbars. */
	zassert_equal(bat_soc_bars(100, 4), 4U, "100%% -> full");
	zassert_equal(bat_soc_bars(127, 4), 4U, ">100 clamps to full");
}

ZTEST(battery, test_soc_bars_rounding_4)
{
	/* 4-bar display, rounded: (soc*4 + 50) / 100. */
	zassert_equal(bat_soc_bars(99, 4), 4U, "99%% -> 4");
	zassert_equal(bat_soc_bars(88, 4), 4U, "88%% -> 4");
	zassert_equal(bat_soc_bars(75, 4), 3U, "75%% -> 3");
	zassert_equal(bat_soc_bars(50, 4), 2U, "50%% -> 2");
	zassert_equal(bat_soc_bars(38, 4), 2U, "38%% -> 2");
	zassert_equal(bat_soc_bars(25, 4), 1U, "25%% -> 1");
	zassert_equal(bat_soc_bars(13, 4), 1U, "13%% -> 1");
	zassert_equal(bat_soc_bars(12, 4), 0U, "12%% -> 0 (below half a bar)");
	zassert_equal(bat_soc_bars(1, 4), 0U, "1%% -> 0");
}

ZTEST(battery, test_soc_bars_rounding_10)
{
	/* A finer 10-bar scale to catch a scale-dependent rounding bug. */
	zassert_equal(bat_soc_bars(55, 10), 6U, "55%% -> 6 of 10");
	zassert_equal(bat_soc_bars(5, 10), 1U, "5%% -> 1 of 10");
	zassert_equal(bat_soc_bars(4, 10), 0U, "4%% -> 0 of 10");
	zassert_equal(bat_soc_bars(100, 10), 10U, "100%% -> 10 of 10");
}

ZTEST(battery, test_power_present)
{
	/* USB / external 5V present. */
	zassert_true(bat_power_present(5000), "5000 mV -> present");
	zassert_true(bat_power_present(BAT_VIN_PRESENT_MV), "threshold -> present");
	zassert_true(bat_power_present(4800), "~5V -> present");

	/* On battery (VIN low) or read error. */
	zassert_false(bat_power_present(BAT_VIN_PRESENT_MV - 1), "below threshold -> absent");
	zassert_false(bat_power_present(0), "0 mV -> absent");
	zassert_false(bat_power_present(-1), "read error -> absent");
}

ZTEST_SUITE(battery, NULL, NULL, NULL, NULL, NULL);
