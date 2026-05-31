/*
 * Copyright (c) 2026 Hsiu-Chi Tsai
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2C emulator for the M5Stack M5PM1 PMIC behind the `m5stack,m5pm1` MFD
 * parent node, for native_sim driver tests.
 *
 * Backs the on-board MFD path (mfd_m5pm1.c + gpio_m5pm1.c + adc_m5pm1.c + a
 * regulator-fixed), so the wake-retry / idle-sleep-disable logic in mfd_m5pm1.c
 * (a local delta vs Zephyr PR #109961) can be exercised under ztest.
 *
 * Implements a 256-byte register file. Registers that the drivers are expected
 * to change are seeded by the TEST to the OPPOSITE of the expected post-init
 * state so an assertion can only pass if the driver actually performs the
 * write. The emulator records every register write (address AND value) so the
 * test can assert e.g. reg 0x09 was written 0x00, and supports failing the
 * first N I2C transactions to exercise the MFD wake-retry.
 *
 * NOTE: this is a dumb byte-store, not an independent oracle of register
 * meaning. It verifies the drivers write what we *think* they should; only
 * on-hardware validation (the LCD lighting up, an I2C bus trace vs the vendor
 * library) confirms the register meanings are correct.
 */

#define DT_DRV_COMPAT m5stack_m5pm1

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(emul_m5pm1_mfd, CONFIG_MFD_LOG_LEVEL);

#define M5PM1_EMUL_WLOG_LEN 64

struct m5pm1_emul_data {
	uint8_t regs[256];
	/* Fault injection: when > 0, the next N transfers return -EIO. */
	int fail_remaining;
	/* Ordered log of (register address, value) pairs that were written. */
	uint8_t wlog_reg[M5PM1_EMUL_WLOG_LEN];
	uint8_t wlog_val[M5PM1_EMUL_WLOG_LEN];
	int wcount;
};

/* Test backend hooks (declared extern in the test). */
void emul_m5pm1_set_fail(const struct emul *target, int n)
{
	struct m5pm1_emul_data *data = target->data;

	data->fail_remaining = n;
}

void emul_m5pm1_reset_log(const struct emul *target)
{
	struct m5pm1_emul_data *data = target->data;

	data->wcount = 0;
}

uint8_t emul_m5pm1_peek(const struct emul *target, uint8_t reg)
{
	struct m5pm1_emul_data *data = target->data;

	return data->regs[reg];
}

void emul_m5pm1_poke(const struct emul *target, uint8_t reg, uint8_t val)
{
	struct m5pm1_emul_data *data = target->data;

	data->regs[reg] = val;
}

/*
 * Return the value most recently WRITTEN to `reg` since the last reset_log(),
 * or -1 if the register was never written. Lets a test assert the exact value
 * a driver wrote (e.g. reg 0x09 == 0x00), not just the final byte-store state.
 */
int emul_m5pm1_last_write(const struct emul *target, uint8_t reg)
{
	struct m5pm1_emul_data *data = target->data;

	for (int i = data->wcount - 1; i >= 0 && i < M5PM1_EMUL_WLOG_LEN; i--) {
		if (data->wlog_reg[i] == reg) {
			return data->wlog_val[i];
		}
	}

	return -1;
}

static int m5pm1_emul_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs,
			       int addr)
{
	struct m5pm1_emul_data *data = target->data;

	ARG_UNUSED(addr);
	__ASSERT_NO_MSG(msgs && num_msgs);

	if (data->fail_remaining > 0) {
		data->fail_remaining--;
		return -EIO;
	}

	if (num_msgs == 1) {
		/* Write transaction: buf = [reg, value]; only len 2 is valid. */
		struct i2c_msg *m = &msgs[0];

		if ((m->flags & I2C_MSG_READ) || m->len != 2) {
			return -EIO;
		}
		data->regs[m->buf[0]] = m->buf[1];
		if (data->wcount < M5PM1_EMUL_WLOG_LEN) {
			data->wlog_reg[data->wcount] = m->buf[0];
			data->wlog_val[data->wcount] = m->buf[1];
		}
		data->wcount++;
		LOG_DBG("W reg=0x%02x val=0x%02x", m->buf[0], m->buf[1]);
		return 0;
	}

	if (num_msgs == 2) {
		/* Write reg address, then read N bytes. */
		struct i2c_msg *w = &msgs[0];
		struct i2c_msg *r = &msgs[1];
		uint8_t reg;

		if ((w->flags & I2C_MSG_READ) || w->len != 1 || !(r->flags & I2C_MSG_READ)) {
			return -EIO;
		}
		reg = w->buf[0];
		for (uint32_t i = 0; i < r->len; i++) {
			r->buf[i] = data->regs[(uint8_t)(reg + i)];
		}
		return 0;
	}

	return -EIO;
}

static const struct i2c_emul_api m5pm1_emul_api = {
	.transfer = m5pm1_emul_transfer,
};

static int m5pm1_emul_init(const struct emul *target, const struct device *parent)
{
	struct m5pm1_emul_data *data = target->data;

	ARG_UNUSED(parent);

	memset(data->regs, 0, sizeof(data->regs));
	data->wcount = 0;

	/* Seed the ID/model so mfd_m5pm1_init() accepts the device. */
	data->regs[0x00] = 0x50; /* DEVICE_ID */
	data->regs[0x01] = 0x20; /* DEVICE_MODEL */

	/*
	 * This emulator init runs at the i2c emul-controller priority (50),
	 * BEFORE the MFD (80) / adc (81) / gpio (82) / regulator-fixed (83)
	 * devices init at boot. So everything below seeds the boot starting
	 * state the drivers then act on, with each driver-touched register set
	 * to the OPPOSITE of its expected post-init value -- a driver that fails
	 * to write leaves the opposite here and the matching test assertion fails.
	 */

	/*
	 * Wake-drop: fail exactly the FIRST I2C transfer. mfd_m5pm1_init() must
	 * absorb it via its 3x ID-read retry; if the retry were removed the MFD
	 * would fail to init and every dependent device would be not-ready.
	 */
	data->fail_remaining = 1;

	/* Idle-sleep reg: opposite of the expected post-init 0x00. */
	data->regs[0x09] = 0xFF;

	/*
	 * PYG2 (GPIO2) registers, opposite of the expected enabled state:
	 *   FUNC0 [5:4] = alt-func (driver must clear -> plain GPIO)
	 *   MODE  bit2  = input    (driver must set   -> output)
	 *   OUT   bit2  = low      (driver must set   -> high)
	 *   DRV   bit2  = open-drn (driver must clear -> push-pull)
	 */
	data->regs[0x16] = 0xFF;   /* GPIO_FUNC0 */
	data->regs[0x10] = 0x00;   /* GPIO_MODE  */
	data->regs[0x11] = 0x00;   /* GPIO_OUT   */
	data->regs[0x13] = BIT(2); /* GPIO_DRV   */

	/* VBAT register pair (0x22/0x23) seeded to a known 4180 mV, LE. */
	data->regs[0x22] = 4180U & 0xFFU;
	data->regs[0x23] = (4180U >> 8) & 0xFFU;

	return 0;
}

#define M5PM1_EMUL(n)                                                                              \
	static struct m5pm1_emul_data m5pm1_emul_data_##n;                                         \
	EMUL_DT_INST_DEFINE(n, m5pm1_emul_init, &m5pm1_emul_data_##n, NULL, &m5pm1_emul_api, NULL)

DT_INST_FOREACH_STATUS_OKAY(M5PM1_EMUL)
