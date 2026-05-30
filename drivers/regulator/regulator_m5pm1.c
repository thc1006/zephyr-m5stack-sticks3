/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * M5Stack M5PM1 PMIC single-rail regulator (I2C 0x6E).
 *
 * The M5PM1 gates the M5StickS3 peripheral power domains through its GPIOs
 * (PYG0..PYG4). The LCD / MIC / SPK domain ("L3B") is a board-level load switch
 * enabled by driving M5PM1 GPIO2 (PYG2) high. This driver models one such
 * GPIO-switched rail as a Zephyr regulator so a consumer (the display) is
 * powered via `regulator-boot-on` before it is initialized.
 *
 * NOTE: interim out-of-tree driver. The canonical upstream M5PM1 support (MFD +
 * gpio + adc + regulator) is being introduced by Zephyr PR #109961; the StickS3
 * board should reuse that once merged rather than this single-rail regulator.
 * See docs/07_UPSTREAM_PLAN.md.
 *
 * Register map and the enable sequence are taken from the M5PM1 datasheet
 * (M5PM1 Chip User Manual V1.9) cross-checked against the vendor M5GFX source
 * (src/M5GFX.cpp, board_M5StickS3) and the StickS3 low-power docs. Per-pin GPIO
 * bit layout:
 *   0x10 GPIO_MODE  : 1 bit/pin  (1 = output)
 *   0x11 GPIO_OUT   : 1 bit/pin  (1 = high)
 *   0x13 GPIO_DRV   : 1 bit/pin  (0 = push-pull; reset default 0x1F = open-drain)
 *   0x16 GPIO_FUNC0 : 2 bits/pin for GPIO0..3 (00 = plain GPIO)
 *   0x17 GPIO_FUNC1 : 2 bits     for GPIO4
 *   0x09 I2C_CFG    : write 0x00 to disable idle-sleep (keep PMIC responsive)
 *   0x00 DEVICE_ID  : reads 0x50
 */

#define DT_DRV_COMPAT m5stack_m5pm1_l3b_regulator

#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(regulator_m5pm1, CONFIG_REGULATOR_LOG_LEVEL);

#define M5PM1_REG_DEVICE_ID  0x00U
#define M5PM1_DEVICE_ID      0x50U
#define M5PM1_REG_I2C_CFG    0x09U
#define M5PM1_REG_GPIO_MODE  0x10U
#define M5PM1_REG_GPIO_OUT   0x11U
#define M5PM1_REG_GPIO_DRV   0x13U
#define M5PM1_REG_GPIO_FUNC0 0x16U
#define M5PM1_REG_GPIO_FUNC1 0x17U

struct regulator_m5pm1_config {
	struct regulator_common_config common;
	struct i2c_dt_spec i2c;
	uint8_t gpio;
};

struct regulator_m5pm1_data {
	struct regulator_common_data common;
};

/*
 * enable()/disable() do a multi-register read-modify-write. The regulator core
 * already serializes these per device with its own mutex (regulator_common.c),
 * and this is the only driver touching the M5PM1, so no extra lock is needed.
 * If other M5PM1 function drivers are added later (charger, GPIO), promote this
 * to an MFD-level shared lock.
 */
static int regulator_m5pm1_enable(const struct device *dev)
{
	const struct regulator_m5pm1_config *cfg = dev->config;
	const uint8_t g = cfg->gpio;
	const uint8_t bit = BIT(g);
	/* GPIO_FUNC0 holds 2 bits/pin for GPIO0..3; GPIO4 is in GPIO_FUNC1[1:0]. */
	const uint8_t func_reg = (g < 4U) ? M5PM1_REG_GPIO_FUNC0 : M5PM1_REG_GPIO_FUNC1;
	const uint8_t func_mask = (g < 4U) ? (uint8_t)(0x3U << (g * 2U)) : 0x3U;
	int ret;

	/* Plain GPIO function (clear the pin's 2-bit FUNC field). */
	ret = i2c_reg_update_byte_dt(&cfg->i2c, func_reg, func_mask, 0U);
	if (ret < 0) {
		return ret;
	}
	/* Output direction. */
	ret = i2c_reg_update_byte_dt(&cfg->i2c, M5PM1_REG_GPIO_MODE, bit, bit);
	if (ret < 0) {
		return ret;
	}
	/* Push-pull drive (default is open-drain). */
	ret = i2c_reg_update_byte_dt(&cfg->i2c, M5PM1_REG_GPIO_DRV, bit, 0U);
	if (ret < 0) {
		return ret;
	}
	/* Drive high -> rail ON. */
	return i2c_reg_update_byte_dt(&cfg->i2c, M5PM1_REG_GPIO_OUT, bit, bit);
}

static int regulator_m5pm1_disable(const struct device *dev)
{
	const struct regulator_m5pm1_config *cfg = dev->config;

	return i2c_reg_update_byte_dt(&cfg->i2c, M5PM1_REG_GPIO_OUT, BIT(cfg->gpio), 0U);
}

static DEVICE_API(regulator, regulator_m5pm1_api) = {
	.enable = regulator_m5pm1_enable,
	.disable = regulator_m5pm1_disable,
};

static int regulator_m5pm1_init(const struct device *dev)
{
	const struct regulator_m5pm1_config *cfg = dev->config;
	uint8_t val = 0U;
	int ret = -EIO;

	regulator_common_data_init(dev);

	if (!i2c_is_ready_dt(&cfg->i2c)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}

	/* The M5PM1 can idle-sleep; the first transaction after wake may be
	 * dropped, so retry the ID read a few times.
	 */
	for (int i = 0; i < 3; i++) {
		ret = i2c_reg_read_byte_dt(&cfg->i2c, M5PM1_REG_DEVICE_ID, &val);
		if (ret == 0) {
			break;
		}
		k_msleep(50);
	}
	if (ret < 0) {
		LOG_ERR("M5PM1 not responding on I2C: %d", ret);
		return ret;
	}
	if (val != M5PM1_DEVICE_ID) {
		LOG_WRN("Unexpected M5PM1 device id 0x%02x (expected 0x%02x)", val,
			M5PM1_DEVICE_ID);
	}

	/* Keep the PMIC awake during display bring-up. The first write after an
	 * idle-sleep wake can be dropped, so check and retry once.
	 */
	ret = i2c_reg_write_byte_dt(&cfg->i2c, M5PM1_REG_I2C_CFG, 0x00U);
	if (ret < 0) {
		ret = i2c_reg_write_byte_dt(&cfg->i2c, M5PM1_REG_I2C_CFG, 0x00U);
		if (ret < 0) {
			return ret;
		}
	}

	/*
	 * Assert the full enable sequence unconditionally and idempotently so the
	 * rail is push-pull-driven regardless of prior PMIC state, then settle
	 * before the (higher-priority) display init resets the panel. This avoids
	 * the case where the rail reads already-on but is still open-drain / not
	 * settled, in which regulator_common_init would skip enable() + delay.
	 */
	ret = regulator_m5pm1_enable(dev);
	if (ret < 0) {
		LOG_ERR("Failed to enable rail: %d", ret);
		return ret;
	}
	k_msleep(100);

	return regulator_common_init(dev, true);
}

#define REGULATOR_M5PM1_DEFINE(inst)                                                                \
	static struct regulator_m5pm1_data data_##inst;                                             \
	static const struct regulator_m5pm1_config config_##inst = {                               \
		.common = REGULATOR_DT_INST_COMMON_CONFIG_INIT(inst),                               \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                  \
		.gpio = DT_INST_PROP(inst, gpio),                                                   \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, regulator_m5pm1_init, NULL, &data_##inst, &config_##inst,       \
			      POST_KERNEL, CONFIG_REGULATOR_M5PM1_INIT_PRIORITY,                   \
			      &regulator_m5pm1_api);

DT_INST_FOREACH_STATUS_OKAY(REGULATOR_M5PM1_DEFINE)
