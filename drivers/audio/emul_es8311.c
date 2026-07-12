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
 * NOTE: this is a register byte-store, not a behavioural model of the codec. It
 * verifies that the driver emits the register writes it is expected to; it does
 * not verify that those values are the right ones for real silicon. That comes
 * from the ES8311 user guide and from hardware validation.
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

#define ES8311_REG_RESET    0x00U
#define ES8311_RESET_BITS   0x1FU /* the digital/CMG/master/ADC/DAC resets */
#define ES8311_REG_CHIP_ID1 0xFDU
#define ES8311_REG_CHIP_ID2 0xFEU
#define ES8311_CHIP_ID1     0x83U
#define ES8311_CHIP_ID2     0x11U

struct es8311_emul_data {
	uint8_t regs[256];
	/* Fault injection: when > 0, the next N transfers return -EIO. */
	int fail_remaining;
	/*
	 * Fault injection by position: when >= 0, transfer number `fail_at` fails and
	 * the ones before it succeed. fail_remaining can only break the FIRST transfer
	 * of a sequence, which is the easy case; a driver that writes eight registers
	 * has eight places to be interrupted, and the interesting ones are in the
	 * middle, where the hardware is half reprogrammed.
	 */
	int fail_at;
	/* Ordered log of written register addresses. */
	uint8_t wlog[ES8311_EMUL_WLOG_LEN];
	int wcount;
	/*
	 * Concurrency hook. When armed, the transfer blocks just before it applies
	 * a write to `pause_reg`, in the calling thread's own context, and signals
	 * `reached`. It resumes when a test gives `release`. That lets a test park
	 * one caller in the middle of a register sequence and drive a second one
	 * into it, which is the only way to observe whether the driver holds its
	 * lock across the whole sequence or merely across the read of its cache.
	 */
	struct k_sem reached;
	struct k_sem release;
	uint8_t pause_reg;
	bool pause_armed;
};

/* Test backend hooks (declared extern in the test). */
void emul_es8311_set_fail(const struct emul *target, int n)
{
	struct es8311_emul_data *data = target->data;

	data->fail_remaining = n;
}

/*
 * Fail transfer number `idx` (0-based), letting the ones before it through. Pass a
 * negative value to disarm. Self-disarming: it fires once.
 */
void emul_es8311_fail_at(const struct emul *target, int idx)
{
	struct es8311_emul_data *data = target->data;

	data->fail_at = idx;
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
 * Arm the concurrency hook: the next write to `reg` blocks inside the transfer,
 * in the caller's own thread, until emul_es8311_release() is called.
 */
void emul_es8311_pause_before(const struct emul *target, uint8_t reg)
{
	struct es8311_emul_data *data = target->data;

	k_sem_init(&data->reached, 0, 1);
	k_sem_init(&data->release, 0, 1);
	data->pause_reg = reg;
	data->pause_armed = true;
}

/* Block until a caller has actually parked on the armed register. */
int emul_es8311_wait_paused(const struct emul *target, k_timeout_t timeout)
{
	struct es8311_emul_data *data = target->data;

	return k_sem_take(&data->reached, timeout);
}

/* Let the parked caller finish its write, and disarm the hook. */
void emul_es8311_release(const struct emul *target)
{
	struct es8311_emul_data *data = target->data;

	data->pause_armed = false;
	k_sem_give(&data->release);
}

/*
 * Override the chip-id registers (0xFD/0xFE) so a test can exercise the
 * driver's rejection of an unexpected identity. The driver reads
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

	if (data->fail_at >= 0) {
		if (data->fail_at == 0) {
			data->fail_at = -1;
			return -EIO;
		}
		data->fail_at--;
	}

	if (num_msgs == 1) {
		/* Write transaction: buf = [reg, value]; only len 2 is valid. */
		struct i2c_msg *m = &msgs[0];

		if ((m->flags & I2C_MSG_READ) || m->len != 2) {
			return -EIO;
		}

		/*
		 * Park here, still in the caller's thread and therefore still holding
		 * whatever locks the caller holds, so a test can drive a second caller
		 * into the middle of this register sequence.
		 */
		if (data->pause_armed && m->buf[0] == data->pause_reg) {
			k_sem_give(&data->reached);
			(void)k_sem_take(&data->release, K_FOREVER);
		}

		/*
		 * Model the register-file reset. The low five bits of register 0x00 are
		 * the digital, clock-manager, master, ADC and DAC resets; asserting them
		 * clears the writable register file on the real part, and the chip-id
		 * registers survive it. CSM_ON (bit 7) is a different bit in the same
		 * register and resets nothing.
		 *
		 * Without this the emulator cannot tell a real reset from a CSM_ON write,
		 * and a test claiming to check that init() resets the chip would be
		 * checking nothing at all.
		 */
		if (m->buf[0] == ES8311_REG_RESET && (m->buf[1] & ES8311_RESET_BITS) != 0U) {
			uint8_t id1 = data->regs[ES8311_REG_CHIP_ID1];
			uint8_t id2 = data->regs[ES8311_REG_CHIP_ID2];

			memset(data->regs, 0, sizeof(data->regs));
			data->regs[ES8311_REG_CHIP_ID1] = id1;
			data->regs[ES8311_REG_CHIP_ID2] = id2;
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
	data->fail_at = -1; /* NOT 0: 0 means "fail transfer 0" */
	data->wcount = 0;

	/* Chip identity registers. */
	data->regs[ES8311_REG_CHIP_ID1] = ES8311_CHIP_ID1;
	data->regs[ES8311_REG_CHIP_ID2] = ES8311_CHIP_ID2;

	return 0;
}

#define ES8311_EMUL(n)                                                                             \
	static struct es8311_emul_data es8311_emul_data_##n;                                       \
	EMUL_DT_INST_DEFINE(n, es8311_emul_init, &es8311_emul_data_##n, NULL, &es8311_emul_api,    \
			    NULL)

DT_INST_FOREACH_STATUS_OKAY(ES8311_EMUL)
