/*
 * Copyright (c) 2026 Hsiu-Chi Tsai
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2C emulator for the Everest ES8311 audio codec, for native_sim driver tests.
 *
 * Implements a 256-byte register file. The chip-id registers (0xFD/0xFE) return
 * the ES8311 identity bytes 0x83/0x11. All other registers default to 0x00 and
 * tests seed the registers the driver is expected to program to the OPPOSITE of
 * the expected post-configure value, so an assertion can only pass if the driver
 * actually performs the write. The emulator also records the ordered sequence of
 * register writes so tests can verify the configure() ordering.
 *
 * NOTE: this is a dumb byte-store, not an independent oracle of register
 * meaning. It verifies the driver writes what we *think* it should; only
 * on-hardware validation (audio out of the StickS3 speaker, an I2C trace vs
 * M5GFX/M5Unified) confirms those register meanings are correct.
 */

#define DT_DRV_COMPAT everest_es8311

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(emul_es8311, CONFIG_AUDIO_CODEC_LOG_LEVEL);

#define ES8311_EMUL_WLOG_LEN 64

#define ES8311_REG_CHIP_ID1 0xFDU
#define ES8311_REG_CHIP_ID2 0xFEU
#define ES8311_CHIP_ID1     0x83U
#define ES8311_CHIP_ID2     0x11U

struct es8311_emul_data {
	uint8_t regs[256];
	/* Fault injection: when > 0, the next N transfers return -EIO. */
	int fail_remaining;
	/* Ordered log of written register addresses. */
	uint8_t wlog[ES8311_EMUL_WLOG_LEN];
	int wcount;
};

/* Test backend hooks (declared extern in the test). */
void emul_es8311_set_fail(const struct emul *target, int n)
{
	struct es8311_emul_data *data = target->data;

	data->fail_remaining = n;
}

void emul_es8311_reset_log(const struct emul *target)
{
	struct es8311_emul_data *data = target->data;

	data->wcount = 0;
}

int emul_es8311_write_count(const struct emul *target)
{
	struct es8311_emul_data *data = target->data;

	return data->wcount;
}

int emul_es8311_write_at(const struct emul *target, int idx)
{
	struct es8311_emul_data *data = target->data;

	if (idx < 0 || idx >= data->wcount || idx >= ES8311_EMUL_WLOG_LEN) {
		return -1;
	}
	return data->wlog[idx];
}

/*
 * Override the chip-id registers (0xFD/0xFE) so a test can exercise the
 * driver's warn-and-continue path on an unexpected identity. The driver reads
 * these in init() via i2c_reg_read_byte_dt().
 */
void emul_es8311_set_chip_id(const struct emul *target, uint8_t id1, uint8_t id2)
{
	struct es8311_emul_data *data = target->data;

	data->regs[ES8311_REG_CHIP_ID1] = id1;
	data->regs[ES8311_REG_CHIP_ID2] = id2;
}

static int es8311_emul_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs,
				int addr)
{
	struct es8311_emul_data *data = target->data;

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
		if (data->wcount < ES8311_EMUL_WLOG_LEN) {
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

static const struct i2c_emul_api es8311_emul_api = {
	.transfer = es8311_emul_transfer,
};

static int es8311_emul_init(const struct emul *target, const struct device *parent)
{
	struct es8311_emul_data *data = target->data;

	ARG_UNUSED(parent);

	memset(data->regs, 0, sizeof(data->regs));
	data->fail_remaining = 0;
	data->wcount = 0;

	/* Chip identity registers. */
	data->regs[ES8311_REG_CHIP_ID1] = ES8311_CHIP_ID1;
	data->regs[ES8311_REG_CHIP_ID2] = ES8311_CHIP_ID2;

	return 0;
}

#define ES8311_EMUL(n)                                                                             \
	static struct es8311_emul_data es8311_emul_data_##n;                                        \
	EMUL_DT_INST_DEFINE(n, es8311_emul_init, &es8311_emul_data_##n, NULL, &es8311_emul_api,     \
			    NULL)

DT_INST_FOREACH_STATUS_OKAY(ES8311_EMUL)
