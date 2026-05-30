/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2C emulator for the M5Stack M5PM1 PMIC, for native_sim driver tests.
 *
 * Implements a 256-byte register file. Registers that the driver is expected to
 * change are seeded to the OPPOSITE of the expected post-enable state (e.g.
 * FUNC0 = 0xFF alt-function, I2C_CFG non-zero, DRV open-drain) so a test can
 * only pass if the driver actually performs the write. The emulator also
 * supports fault injection and records the ordered sequence of register writes
 * so tests can verify error handling and write ordering.
 *
 * NOTE: this is a dumb byte-store, not an independent oracle of register
 * meaning. It verifies the driver writes what we *think* it should; only
 * on-hardware validation (the LCD lighting up, an I2C bus trace vs M5GFX)
 * confirms those register meanings are correct.
 */

#define DT_DRV_COMPAT m5stack_m5pm1_l3b_regulator

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(emul_m5pm1, CONFIG_REGULATOR_LOG_LEVEL);

#define M5PM1_EMUL_WLOG_LEN 32

struct m5pm1_emul_data {
	uint8_t regs[256];
	/* Fault injection: when > 0, the next N transfers return -EIO. */
	int fail_remaining;
	/* Ordered log of written register addresses. */
	uint8_t wlog[M5PM1_EMUL_WLOG_LEN];
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

int emul_m5pm1_write_count(const struct emul *target)
{
	struct m5pm1_emul_data *data = target->data;

	return data->wcount;
}

int emul_m5pm1_write_at(const struct emul *target, int idx)
{
	struct m5pm1_emul_data *data = target->data;

	if (idx < 0 || idx >= data->wcount || idx >= M5PM1_EMUL_WLOG_LEN) {
		return -1;
	}
	return data->wlog[idx];
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
			data->wlog[data->wcount] = m->buf[0];
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
	data->fail_remaining = 0;
	data->wcount = 0;

	/* Info registers (datasheet defaults). */
	data->regs[0x00] = 0x50; /* DEVICE_ID */
	data->regs[0x01] = 0x20; /* DEVICE_MODEL */
	data->regs[0x02] = 0x05; /* HW_REV */
	data->regs[0x03] = 0x06; /* SW_REV */
	data->regs[0x06] = 0x17; /* PWR_CFG */

	/*
	 * Seed driver-touched registers to the OPPOSITE of the expected
	 * post-enable state, with neighbour (PYG3) bits set, so every assertion
	 * requires a real driver write and neighbour-preservation is testable.
	 */
	data->regs[0x09] = 0x42; /* I2C_CFG non-zero -> driver must clear to 0 */
	data->regs[0x10] = 0x08; /* GPIO_MODE: PYG3 set, PYG2 clear */
	data->regs[0x11] = 0x08; /* GPIO_OUT:  PYG3 set, PYG2 clear */
	data->regs[0x13] = 0x1F; /* GPIO_DRV:  all open-drain (PYG2 must clear) */
	data->regs[0x16] = 0xFF; /* GPIO_FUNC0: all alt-func (PYG2 [5:4] must clear) */
	data->regs[0x17] = 0x03; /* GPIO_FUNC1 */
	return 0;
}

#define M5PM1_EMUL(n)                                                                               \
	static struct m5pm1_emul_data m5pm1_emul_data_##n;                                          \
	EMUL_DT_INST_DEFINE(n, m5pm1_emul_init, &m5pm1_emul_data_##n, NULL, &m5pm1_emul_api, NULL)

DT_INST_FOREACH_STATUS_OKAY(M5PM1_EMUL)
