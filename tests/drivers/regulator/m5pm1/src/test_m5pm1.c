/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/sys/util.h>

#define REG_NODE DT_NODELABEL(lcd_power)

static const struct device *const reg = DEVICE_DT_GET(REG_NODE);
static const struct i2c_dt_spec pmic = I2C_DT_SPEC_GET(REG_NODE);
static const struct emul *const emul = EMUL_DT_GET(REG_NODE);

/* Emulator test backend (defined in emul_m5pm1.c). */
extern void emul_m5pm1_set_fail(const struct emul *target, int n);
extern void emul_m5pm1_reset_log(const struct emul *target);
extern int emul_m5pm1_write_count(const struct emul *target);
extern int emul_m5pm1_write_at(const struct emul *target, int idx);

/* M5PM1 register map (subset used by the driver). */
#define M5PM1_GPIO_MODE  0x10
#define M5PM1_GPIO_OUT   0x11
#define M5PM1_GPIO_DRV   0x13
#define M5PM1_GPIO_FUNC0 0x16
#define M5PM1_I2C_CFG    0x09
#define PYG2             BIT(2)
#define PYG3             BIT(3)
#define FUNC0_GPIO2_MASK (0x3U << 4) /* GPIO2 occupies FUNC0 bits [5:4] */
#define FUNC0_GPIO3_MASK (0x3U << 6) /* GPIO3 (neighbour) bits [7:6] */

static uint8_t reg_get(uint8_t r)
{
	uint8_t v = 0xffU;

	zassert_ok(i2c_reg_read_byte_dt(&pmic, r, &v), "i2c read of 0x%02x failed", r);
	return v;
}

static void reg_put(uint8_t r, uint8_t v)
{
	zassert_ok(i2c_reg_write_byte_dt(&pmic, r, v), "i2c write of 0x%02x failed", r);
}

/*
 * After boot (regulator-boot-on), the driver must have driven the L3B rail
 * (PYG2) high using the full M5GFX sequence. The emulator seeds every touched
 * register to the opposite state, so each assertion requires a real write.
 */
ZTEST(m5pm1, test_boot_on_enables_rail)
{
	zassert_true(device_is_ready(reg), "regulator not ready");
	zassert_true(regulator_is_enabled(reg), "rail should be enabled at boot");

	zassert_true(reg_get(M5PM1_GPIO_OUT) & PYG2, "GPIO_OUT bit2 should be high");
	zassert_true(reg_get(M5PM1_GPIO_MODE) & PYG2, "GPIO_MODE bit2 should be output");
	zassert_false(reg_get(M5PM1_GPIO_DRV) & PYG2, "GPIO_DRV bit2 should be push-pull");
	zassert_equal(reg_get(M5PM1_GPIO_FUNC0) & FUNC0_GPIO2_MASK, 0U,
		      "FUNC0 GPIO2 field should be cleared to plain GPIO");
	zassert_equal(reg_get(M5PM1_I2C_CFG), 0U, "idle-sleep should be disabled");
}

/*
 * enable() must re-assert the FULL sequence idempotently. Corrupt MODE/DRV/FUNC0
 * after disable, then enable, and confirm all are restored (not just OUT).
 */
ZTEST(m5pm1, test_enable_reasserts_full_sequence)
{
	zassert_ok(regulator_disable(reg));

	/* Corrupt the rail config as if prior firmware left it wrong. */
	reg_put(M5PM1_GPIO_MODE, 0x00);  /* PYG2 input */
	reg_put(M5PM1_GPIO_DRV, 0x1F);   /* PYG2 open-drain */
	reg_put(M5PM1_GPIO_FUNC0, 0xFF); /* PYG2 alt-func */

	zassert_ok(regulator_enable(reg));

	zassert_true(reg_get(M5PM1_GPIO_OUT) & PYG2, "OUT bit2 set");
	zassert_true(reg_get(M5PM1_GPIO_MODE) & PYG2, "MODE re-asserted to output");
	zassert_false(reg_get(M5PM1_GPIO_DRV) & PYG2, "DRV re-asserted to push-pull");
	zassert_equal(reg_get(M5PM1_GPIO_FUNC0) & FUNC0_GPIO2_MASK, 0U,
		      "FUNC0 re-asserted to plain GPIO");
}

/* disable() clears only PYG2; enable() restores it; refcount tracks state. */
ZTEST(m5pm1, test_disable_then_enable)
{
	zassert_ok(regulator_disable(reg), "disable failed");
	zassert_false(reg_get(M5PM1_GPIO_OUT) & PYG2, "OUT bit2 should clear on disable");
	zassert_false(regulator_is_enabled(reg), "rail should report disabled");

	zassert_ok(regulator_enable(reg), "enable failed");
	zassert_true(reg_get(M5PM1_GPIO_OUT) & PYG2, "OUT bit2 should set on enable");
	zassert_true(regulator_is_enabled(reg), "rail should report enabled");
}

/*
 * The per-pin RMW must not disturb neighbour GPIOs. Set PYG3 bits everywhere,
 * cycle the rail, and confirm PYG3 survives in OUT/MODE/DRV/FUNC0 (catches a
 * mask/shift bug that would clobber a sibling).
 */
ZTEST(m5pm1, test_neighbour_pyg3_preserved)
{
	zassert_ok(regulator_disable(reg));
	reg_put(M5PM1_GPIO_OUT, PYG3);
	reg_put(M5PM1_GPIO_MODE, PYG3);
	reg_put(M5PM1_GPIO_DRV, PYG3);
	reg_put(M5PM1_GPIO_FUNC0, FUNC0_GPIO3_MASK); /* PYG3 alt-func set */

	zassert_ok(regulator_enable(reg));

	zassert_true(reg_get(M5PM1_GPIO_OUT) & PYG3, "OUT PYG3 must be preserved");
	zassert_true(reg_get(M5PM1_GPIO_MODE) & PYG3, "MODE PYG3 must be preserved");
	zassert_true(reg_get(M5PM1_GPIO_DRV) & PYG3, "DRV PYG3 must be preserved");
	zassert_equal(reg_get(M5PM1_GPIO_FUNC0) & FUNC0_GPIO3_MASK, FUNC0_GPIO3_MASK,
		      "FUNC0 GPIO3 field must be preserved");
}

/*
 * Write ordering matters: the rail must reach push-pull output BEFORE being
 * driven high. Verify GPIO_OUT (0x11) is the LAST register written by enable(),
 * and GPIO_DRV (0x13) is written before it.
 */
ZTEST(m5pm1, test_enable_write_order)
{
	int n, last, drv_idx = -1, out_idx = -1;

	zassert_ok(regulator_disable(reg));
	/* Corrupt MODE/DRV/FUNC0 so the RMW writes all actually fire (an
	 * unchanged value is skipped by i2c_reg_update_byte).
	 */
	reg_put(M5PM1_GPIO_MODE, 0x00);
	reg_put(M5PM1_GPIO_DRV, 0x1F);
	reg_put(M5PM1_GPIO_FUNC0, 0xFF);
	emul_m5pm1_reset_log(emul);
	zassert_ok(regulator_enable(reg));

	n = emul_m5pm1_write_count(emul);
	zassert_true(n >= 4, "enable should emit FUNC/MODE/DRV/OUT writes (got %d)", n);

	for (int i = 0; i < n; i++) {
		int r = emul_m5pm1_write_at(emul, i);

		if (r == M5PM1_GPIO_DRV) {
			drv_idx = i;
		}
		if (r == M5PM1_GPIO_OUT) {
			out_idx = i;
		}
	}
	last = emul_m5pm1_write_at(emul, n - 1);
	zassert_equal(last, M5PM1_GPIO_OUT, "GPIO_OUT must be the last write of enable()");
	zassert_true(drv_idx >= 0 && drv_idx < out_idx,
		     "GPIO_DRV (push-pull) must be written before GPIO_OUT (high)");
}

/* I2C failure on the enable path must propagate as an error (not silent OK). */
ZTEST(m5pm1, test_enable_propagates_i2c_error)
{
	zassert_ok(regulator_disable(reg));
	emul_m5pm1_set_fail(emul, 1); /* fail the next transfer */
	zassert_true(regulator_enable(reg) < 0, "enable must return an error on I2C failure");

	emul_m5pm1_set_fail(emul, 0); /* clear injection, restore good state */
	zassert_ok(regulator_enable(reg));
	zassert_true(reg_get(M5PM1_GPIO_OUT) & PYG2);
}

ZTEST_SUITE(m5pm1, NULL, NULL, NULL, NULL, NULL);
