/*
 * Copyright (c) 2026 Hsiu-Chi Tsai
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * native_sim ztest for the ON-BOARD M5PM1 path: the `m5stack,m5pm1` MFD parent
 * (mfd_m5pm1.c) + its GPIO child (gpio_m5pm1.c) + ADC child (adc_m5pm1.c) + a
 * top-level regulator-fixed switched by the MFD GPIO2 (PYG2 = L3B/LCD rail).
 *
 * This covers the path the board actually uses. In particular, the idle-sleep
 * disable + wake-retry that mfd_m5pm1.c adds over Zephyr PR #109961 (a LOCAL
 * DELTA -- see that file's header and docs/07_UPSTREAM_PLAN.md) is exercised
 * here. (The earlier interim `m5stack,m5pm1-l3b-regulator` driver + its ztest
 * were removed once this MFD path was hardware-verified.)
 *
 * Strategy: the MFD/gpio/adc/regulator-fixed devices init at boot in the
 * on-board POST_KERNEL order (I2C 50 < MFD 80 < adc 81 < gpio 82 <
 * regulator-fixed 83). The emulator backend inits earlier (i2c emul controller,
 * priority 50) and there seeds every driver-touched register to the OPPOSITE of
 * its expected post-init value AND injects a single I2C fault. So by the time
 * the suite runs, the registers reflect what the drivers actually did at boot,
 * and each assertion fails if the matching driver write is missing. See
 * drivers/mfd/emul_m5pm1.c for the seeded boot state.
 */

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/sys/util.h>

#define MFD_NODE DT_NODELABEL(m5pm1)

static const struct device *const mfd = DEVICE_DT_GET(MFD_NODE);
static const struct device *const adc = DEVICE_DT_GET(DT_NODELABEL(m5pm1_adc));
static const struct device *const reg = DEVICE_DT_GET(DT_NODELABEL(lcd_power));
static const struct emul *const emul = EMUL_DT_GET(MFD_NODE);

/* Emulator test backend (defined in drivers/mfd/emul_m5pm1.c). */
extern uint8_t emul_m5pm1_peek(const struct emul *target, uint8_t reg);
extern int emul_m5pm1_last_write(const struct emul *target, uint8_t reg);

/* M5PM1 register map (subset). */
#define M5PM1_REG_I2C_CFG 0x09
#define M5PM1_GPIO_MODE   0x10
#define M5PM1_GPIO_OUT    0x11
#define M5PM1_GPIO_DRV    0x13
#define M5PM1_GPIO_FUNC0  0x16

#define PYG2             BIT(2)
#define FUNC0_GPIO2_MASK (0x3U << 4) /* GPIO2 occupies FUNC0 bits [5:4] */

/* VBAT logical channel on the M5PM1 ADC (overlay channel@1). */
#define VBAT_CHANNEL 1
#define VBAT_TEST_MV 4180 /* must match the seed in emul_m5pm1.c */

/*
 * TEST 1: mfd_m5pm1_init() must DISABLE idle-sleep by writing reg 0x09 = 0x00.
 *
 * Load-bearing: the emulator seeds 0x09 = 0xFF (opposite), so a no-op leaves
 * 0xFF in the byte-store; and last_write(0x09) returns -1 if 0x09 was never
 * written. We assert BOTH the recorded write value is 0x00 and the resulting
 * byte-store is 0x00. Delete the reg-0x09 write from mfd_m5pm1.c and this test
 * fails (last_write -> -1, peek -> 0xFF).
 */
ZTEST(m5pm1_mfd, test_mfd_init_disables_idle_sleep)
{
	zassert_true(device_is_ready(mfd), "MFD not ready");

	zassert_equal(emul_m5pm1_last_write(emul, M5PM1_REG_I2C_CFG), 0x00,
		      "init must WRITE idle-sleep reg 0x09 = 0x00 (last_write was %d)",
		      emul_m5pm1_last_write(emul, M5PM1_REG_I2C_CFG));
	zassert_equal(emul_m5pm1_peek(emul, M5PM1_REG_I2C_CFG), 0x00,
		      "idle-sleep reg 0x09 should read 0x00 after init (was seeded 0xFF)");
}

/*
 * TEST 2: the wake-retry absorbs a dropped first transaction.
 *
 * The emulator failed the very first I2C transfer at boot (the MFD's first ID
 * read). The MFD must STILL be ready, because mfd_m5pm1_init() retries the ID
 * read up to 3x. Load-bearing: with the retry removed, that first -EIO would
 * abort init and the device would be not-ready -- failing this assertion. (We
 * also know test 1 passed, proving the retry recovered far enough to complete
 * the full init, not merely the ID read.)
 */
ZTEST(m5pm1_mfd, test_mfd_init_wake_retry)
{
	zassert_true(device_is_ready(mfd),
		     "MFD must be ready despite a dropped first I2C transfer (wake-retry)");
}

/*
 * TEST 3: enabling the regulator-fixed drives PYG2 (L3B/LCD rail) high via the
 * MFD GPIO child, using the full plain-GPIO push-pull output-high sequence.
 *
 * Load-bearing: the emulator seeded FUNC0/MODE/OUT/DRV for PYG2 to the opposite
 * of the expected state, so each bit assertion requires a real driver write.
 * regulator-boot-on auto-enabled the rail at boot.
 */
ZTEST(m5pm1_mfd, test_l3b_enable_drives_pyg2)
{
	zassert_true(device_is_ready(reg), "regulator not ready");
	zassert_true(regulator_is_enabled(reg),
		     "rail should be enabled at boot (regulator-boot-on)");

	zassert_equal(emul_m5pm1_peek(emul, M5PM1_GPIO_FUNC0) & FUNC0_GPIO2_MASK, 0U,
		      "FUNC0 GPIO2 field [5:4] should be cleared to plain GPIO");
	zassert_true(emul_m5pm1_peek(emul, M5PM1_GPIO_MODE) & PYG2,
		     "GPIO_MODE bit2 should be output");
	zassert_false(emul_m5pm1_peek(emul, M5PM1_GPIO_DRV) & PYG2,
		      "GPIO_DRV bit2 should be push-pull (0)");
	zassert_true(emul_m5pm1_peek(emul, M5PM1_GPIO_OUT) & PYG2,
		     "GPIO_OUT bit2 should be driven high");
}

/*
 * TEST 4: the ADC reads VBAT (logical channel 1) back as the seeded millivolts.
 *
 * Load-bearing: the emulator's VBAT register pair was seeded to 4180; the read
 * must surface exactly that. A wrong register, byte order, or channel mapping
 * yields a different value.
 */
ZTEST(m5pm1_mfd, test_adc_vbat_reads_mv)
{
	uint16_t sample = 0;
	struct adc_channel_cfg ch_cfg = {
		.gain = ADC_GAIN_1,
		.reference = ADC_REF_INTERNAL,
		.acquisition_time = ADC_ACQ_TIME_DEFAULT,
		.channel_id = VBAT_CHANNEL,
	};
	struct adc_sequence seq = {
		.channels = BIT(VBAT_CHANNEL),
		.buffer = &sample,
		.buffer_size = sizeof(sample),
		.resolution = 12,
	};

	zassert_true(device_is_ready(adc), "ADC not ready");

	zassert_ok(adc_channel_setup(adc, &ch_cfg), "VBAT channel setup failed");
	zassert_ok(adc_read(adc, &seq), "VBAT adc_read failed");
	zassert_equal(sample, VBAT_TEST_MV, "VBAT should read %d mV, got %u", VBAT_TEST_MV, sample);
}

ZTEST_SUITE(m5pm1_mfd, NULL, NULL, NULL, NULL, NULL);
