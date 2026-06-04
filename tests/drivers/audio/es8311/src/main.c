/*
 * Copyright (c) 2026 Hsiu-Chi Tsai
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/audio/codec.h>
#include <zephyr/sys/util.h>

#define CODEC_NODE DT_NODELABEL(codec)

static const struct device *const codec = DEVICE_DT_GET(CODEC_NODE);
static const struct i2c_dt_spec es = I2C_DT_SPEC_GET(CODEC_NODE);
static const struct emul *const emul = EMUL_DT_GET(CODEC_NODE);

/* Emulator test backend (defined in emul_es8311.c). */
extern void emul_es8311_set_fail(const struct emul *target, int n);
extern void emul_es8311_reset_log(const struct emul *target);
extern int emul_es8311_write_count(const struct emul *target);
extern int emul_es8311_write_at(const struct emul *target, int idx);
extern void emul_es8311_set_chip_id(const struct emul *target, uint8_t id1, uint8_t id2);

/* ES8311 register map (subset exercised by the driver). */
#define ES8311_REG_RESET       0x00
#define ES8311_REG_CLK_MANAGER 0x01
#define ES8311_REG_CLK_DIV1    0x02
#define ES8311_REG_CLK_BCLK    0x06
#define ES8311_REG_CLK_LRCK_L  0x08
#define ES8311_REG_SDP_IN      0x09
#define ES8311_REG_SYSTEM_0D   0x0D
#define ES8311_REG_SYSTEM_12   0x12
#define ES8311_REG_SYSTEM_13   0x13
#define ES8311_REG_DAC_MUTE    0x31
#define ES8311_REG_DAC_VOLUME  0x32
#define ES8311_REG_DAC_EQ      0x37
/* ADC / capture path registers. */
#define ES8311_REG_SDP_OUT     0x0A
#define ES8311_REG_SYSTEM_0E   0x0E
#define ES8311_REG_ADC_PGA     0x14
#define ES8311_REG_ADC_OSR     0x15
#define ES8311_REG_ADC_CTRL    0x16
#define ES8311_REG_ADC_VOLUME  0x17
#define ES8311_REG_ADC_HPF1    0x1B
#define ES8311_REG_ADC_HPF2    0x1C
#define ES8311_REG_ADC_MUX     0x44
#define ES8311_REG_ADC_GP45    0x45
#define ES8311_REG_CHIP_ID1    0xFD
#define ES8311_REG_CHIP_ID2    0xFE

static uint8_t reg_get(uint8_t r)
{
	uint8_t v = 0xa5U;

	zassert_ok(i2c_reg_read_byte_dt(&es, r, &v), "i2c read of 0x%02x failed", r);
	return v;
}

static void reg_put(uint8_t r, uint8_t v)
{
	zassert_ok(i2c_reg_write_byte_dt(&es, r, v), "i2c write of 0x%02x failed", r);
}

/* Build a 16 kHz / 16-bit / MCLK-from-BCLK playback configuration. */
static void make_cfg_16k_16bit(struct audio_codec_cfg *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->mclk_freq = 4096000U; /* BCLK*8 */
	cfg->dai_type = AUDIO_DAI_TYPE_I2S;
	cfg->dai_route = AUDIO_ROUTE_PLAYBACK;
	cfg->dai_cfg.i2s.word_size = AUDIO_PCM_WIDTH_16_BITS;
	cfg->dai_cfg.i2s.channels = 2;
	cfg->dai_cfg.i2s.frame_clk_freq = AUDIO_PCM_RATE_16K;
}

/*
 * The driver reads the chip-id registers (0xFD/0xFE) in init(). The emulator
 * seeds them to 0x83/0x11, so a readable identity proves the bus is wired and
 * init() ran. (init warns-and-continues on mismatch; this asserts the values
 * the driver checks against.)
 */
ZTEST(es8311, test_init_reads_chip_id)
{
	zassert_true(device_is_ready(codec), "codec device not ready");
	zassert_equal(reg_get(ES8311_REG_CHIP_ID1), 0x83U, "chip id1 should read 0x83");
	zassert_equal(reg_get(ES8311_REG_CHIP_ID2), 0x11U, "chip id2 should read 0x11");
}

/*
 * The driver only warns (does not fail) when the chip-id registers do not hold
 * the ES8311 identity. Seed a wrong id (0x00/0x00), force the driver's init()
 * to run again so it re-reads those registers, and assert init() still reports
 * success and the device stays ready (warn-and-continue). This observes the
 * real driver path: device_init() re-invokes es8311_init() -> es8311_check_id()
 * which reads the seeded wrong values over the emulated I2C bus.
 */
ZTEST(es8311, test_init_wrong_chip_id_warns_and_continues)
{
	int ret;

	/* Confirm the device came up cleanly first. */
	zassert_true(device_is_ready(codec), "codec device not ready before re-init");

	/* Seed an obviously-wrong identity. */
	emul_es8311_set_chip_id(emul, 0x00U, 0x00U);
	zassert_equal(reg_get(ES8311_REG_CHIP_ID1), 0x00U, "chip id1 seed failed");
	zassert_equal(reg_get(ES8311_REG_CHIP_ID2), 0x00U, "chip id2 seed failed");

	/* Force the driver init to run again against the wrong identity. */
	codec->state->initialized = false;
	ret = device_init(codec);

	/* Warn-and-continue: init must still succeed and the device be ready. */
	zassert_ok(ret, "init() must succeed despite wrong chip id (got %d)", ret);
	zassert_true(device_is_ready(codec), "device must stay ready after wrong-id init");

	/* Restore the correct identity for the remaining tests. */
	emul_es8311_set_chip_id(emul, 0x83U, 0x11U);
}

/*
 * configure() for 16 kHz / 16-bit must emit the hardware-verified register
 * sequence. Seed every touched register to the opposite of the expected value
 * first so each assertion requires a real write by the driver.
 */
ZTEST(es8311, test_configure_16k_16bit_sequence)
{
	struct audio_codec_cfg cfg;

	/* Poison the registers the sequence is expected to set. */
	reg_put(ES8311_REG_RESET, 0x00);
	reg_put(ES8311_REG_CLK_MANAGER, 0x00);
	reg_put(ES8311_REG_CLK_DIV1, 0x00);
	reg_put(ES8311_REG_CLK_BCLK, 0xFF);
	reg_put(ES8311_REG_CLK_LRCK_L, 0x00);
	reg_put(ES8311_REG_SDP_IN, 0xFF);
	reg_put(ES8311_REG_SYSTEM_0D, 0xFF);
	reg_put(ES8311_REG_SYSTEM_12, 0xFF);
	reg_put(ES8311_REG_SYSTEM_13, 0x00);
	reg_put(ES8311_REG_DAC_EQ, 0x00);
	reg_put(ES8311_REG_DAC_MUTE, 0xFF);

	make_cfg_16k_16bit(&cfg);
	zassert_ok(audio_codec_configure(codec, &cfg), "configure() failed");

	/* Reset released / CSM on. */
	zassert_equal(reg_get(ES8311_REG_RESET), 0x80U, "0x00 should be 0x80");
	/* Clock manager: MCLK from BCLK, clocks on. */
	zassert_equal(reg_get(ES8311_REG_CLK_MANAGER), 0xBFU, "0x01 should be 0xBF");
	/* pre_div=1, pre_multi=x8. */
	zassert_equal(reg_get(ES8311_REG_CLK_DIV1), 0x18U, "0x02 should be 0x18");
	/* bclk divider. */
	zassert_equal(reg_get(ES8311_REG_CLK_BCLK), 0x03U, "0x06 should be 0x03");
	/* LRCK = MCLK/256. */
	zassert_equal(reg_get(ES8311_REG_CLK_LRCK_L), 0xFFU, "0x08 should be 0xFF");
	/* I2S, 16-bit. */
	zassert_equal(reg_get(ES8311_REG_SDP_IN), 0x0CU, "0x09 should be 0x0C");
	/* Analog power up. */
	zassert_equal(reg_get(ES8311_REG_SYSTEM_0D), 0x01U, "0x0D should be 0x01");
	/* DAC power up. */
	zassert_equal(reg_get(ES8311_REG_SYSTEM_12), 0x00U, "0x12 should be 0x00");
	/* DAC out / HP drive. */
	zassert_equal(reg_get(ES8311_REG_SYSTEM_13), 0x10U, "0x13 should be 0x10");
	/* EQ bypass. */
	zassert_equal(reg_get(ES8311_REG_DAC_EQ), 0x08U, "0x37 should be 0x08");
	/* Default DAC volume programmed. */
	zassert_equal(reg_get(ES8311_REG_DAC_VOLUME), 0xC0U, "0x32 should be default 0xC0");
	/* Unmuted at end of configure. */
	zassert_equal(reg_get(ES8311_REG_DAC_MUTE), 0x00U, "0x31 should be unmuted (0x00)");
}

/*
 * Within configure(), reset (0x00) must precede the clock manager (0x01), and
 * the analog power-up (0x0D) must precede the DAC power-up (0x12). Verify the
 * recorded write order.
 */
ZTEST(es8311, test_configure_write_order)
{
	struct audio_codec_cfg cfg;
	int n, reset_idx = -1, clk_idx = -1, ana_idx = -1, dac_idx = -1;

	emul_es8311_reset_log(emul);
	make_cfg_16k_16bit(&cfg);
	zassert_ok(audio_codec_configure(codec, &cfg), "configure() failed");

	n = emul_es8311_write_count(emul);
	zassert_true(n >= 13, "configure should emit the full sequence (got %d)", n);

	for (int i = 0; i < n; i++) {
		int r = emul_es8311_write_at(emul, i);

		if (r == ES8311_REG_RESET && reset_idx < 0) {
			reset_idx = i;
		}
		if (r == ES8311_REG_CLK_MANAGER && clk_idx < 0) {
			clk_idx = i;
		}
		if (r == ES8311_REG_SYSTEM_0D && ana_idx < 0) {
			ana_idx = i;
		}
		if (r == ES8311_REG_SYSTEM_12 && dac_idx < 0) {
			dac_idx = i;
		}
	}

	zassert_true(reset_idx >= 0 && reset_idx < clk_idx,
		     "reset (0x00) must be written before clk manager (0x01)");
	zassert_true(ana_idx >= 0 && ana_idx < dac_idx,
		     "analog power-up (0x0D) must precede DAC power-up (0x12)");
}

/* configure() must reject a non-I2S DAI and an unsupported route. */
ZTEST(es8311, test_configure_rejects_unsupported)
{
	struct audio_codec_cfg cfg;

	make_cfg_16k_16bit(&cfg);
	cfg.dai_type = AUDIO_DAI_TYPE_PCM;
	zassert_equal(audio_codec_configure(codec, &cfg), -ENOTSUP,
		      "non-I2S DAI must be rejected");

	make_cfg_16k_16bit(&cfg);
	cfg.dai_route = AUDIO_ROUTE_BYPASS;
	zassert_equal(audio_codec_configure(codec, &cfg), -ENOTSUP,
		      "bypass route must be rejected (only playback/capture supported)");

	make_cfg_16k_16bit(&cfg);
	cfg.dai_cfg.i2s.frame_clk_freq = AUDIO_PCM_RATE_44P1K;
	zassert_equal(audio_codec_configure(codec, &cfg), -ENOTSUP,
		      "unsupported sample rate must be rejected");
}

/*
 * OUTPUT_VOLUME set_property + apply_properties must write the DAC volume
 * register (0x32). 0 dB maps to code 0xBF. Seed 0x32 to a different value first.
 */
ZTEST(es8311, test_set_volume)
{
	audio_property_value_t val = {.vol = 0};

	reg_put(ES8311_REG_DAC_VOLUME, 0x00);

	zassert_ok(audio_codec_set_property(codec, AUDIO_PROPERTY_OUTPUT_VOLUME,
					    AUDIO_CHANNEL_ALL, val),
		   "set OUTPUT_VOLUME failed");
	zassert_ok(audio_codec_apply_properties(codec), "apply_properties failed");

	zassert_equal(reg_get(ES8311_REG_DAC_VOLUME), 0xBFU, "0 dB should map to 0xBF");
}

/*
 * OUTPUT_MUTE set_property + apply_properties must write the DAC mute field
 * (0x31). Seed it unmuted, mute, verify; then unmute and verify.
 */
ZTEST(es8311, test_set_mute)
{
	audio_property_value_t mute = {.mute = true};
	audio_property_value_t unmute = {.mute = false};

	reg_put(ES8311_REG_DAC_MUTE, 0x00);
	zassert_ok(audio_codec_set_property(codec, AUDIO_PROPERTY_OUTPUT_MUTE,
					    AUDIO_CHANNEL_ALL, mute),
		   "set OUTPUT_MUTE(true) failed");
	zassert_ok(audio_codec_apply_properties(codec), "apply_properties failed");
	zassert_equal(reg_get(ES8311_REG_DAC_MUTE) & 0x60U, 0x60U,
		      "mute field (bit6 DSMMUTE | bit5 DEMMUTE) should be set");

	zassert_ok(audio_codec_set_property(codec, AUDIO_PROPERTY_OUTPUT_MUTE,
					    AUDIO_CHANNEL_ALL, unmute),
		   "set OUTPUT_MUTE(false) failed");
	zassert_ok(audio_codec_apply_properties(codec), "apply_properties failed");
	zassert_equal(reg_get(ES8311_REG_DAC_MUTE) & 0x60U, 0x00U,
		      "mute field (bit6 DSMMUTE | bit5 DEMMUTE) should be clear");
}

/* An unsupported property must be rejected with -ENOTSUP. */
ZTEST(es8311, test_set_property_unsupported)
{
	audio_property_value_t val = {.vol = 0};

	zassert_equal(audio_codec_set_property(codec, AUDIO_PROPERTY_INPUT_VOLUME,
					       AUDIO_CHANNEL_ALL, val),
		      -ENOTSUP, "INPUT_VOLUME is not supported");
}

/* An I2C failure during configure() must propagate as an error. */
ZTEST(es8311, test_configure_propagates_i2c_error)
{
	struct audio_codec_cfg cfg;

	make_cfg_16k_16bit(&cfg);
	emul_es8311_set_fail(emul, 1); /* fail the next transfer */
	zassert_true(audio_codec_configure(codec, &cfg) < 0,
		     "configure() must return an error on I2C failure");

	emul_es8311_set_fail(emul, 0); /* clear injection */
	zassert_ok(audio_codec_configure(codec, &cfg), "configure() should recover");
}

/*
 * configure() with a capture route must additionally emit the ADC register
 * sequence. These ADC values are reference-derived (ESP-ADF es8311_start(ADC))
 * for a single-ended analog MIC1 at 16 kHz / 16-bit; they are hardware-validated
 * at HW-016. The test pins the driver's contract. PLAYBACK_CAPTURE is the
 * acoustic-loopback route, so the DAC sequence must still be emitted too.
 */
ZTEST(es8311, test_configure_capture_sequence)
{
	struct audio_codec_cfg cfg;

	/* Poison the ADC registers the capture path is expected to set. */
	reg_put(ES8311_REG_SDP_OUT, 0xFF);
	reg_put(ES8311_REG_SYSTEM_0E, 0xFF);
	reg_put(ES8311_REG_ADC_PGA, 0x00);
	reg_put(ES8311_REG_ADC_OSR, 0x00);
	reg_put(ES8311_REG_ADC_CTRL, 0x00);
	reg_put(ES8311_REG_ADC_VOLUME, 0x00);
	reg_put(ES8311_REG_ADC_HPF1, 0xFF);
	reg_put(ES8311_REG_ADC_HPF2, 0x00);
	reg_put(ES8311_REG_ADC_MUX, 0xFF);
	reg_put(ES8311_REG_ADC_GP45, 0xFF);

	make_cfg_16k_16bit(&cfg);
	cfg.dai_route = AUDIO_ROUTE_PLAYBACK_CAPTURE;
	zassert_ok(audio_codec_configure(codec, &cfg),
		   "configure(PLAYBACK_CAPTURE) failed");

	/* ADC serial-data-out port: standard I2S, 16-bit (bit6=0 leaves the ADC SDP unmuted). */
	zassert_equal(reg_get(ES8311_REG_SDP_OUT), 0x0CU, "0x0A should be 0x0C");
	/* ADC power up. */
	zassert_equal(reg_get(ES8311_REG_SYSTEM_0E), 0x02U, "0x0E should be 0x02");
	/* Single-ended analog MIC1 + PGA. */
	zassert_equal(reg_get(ES8311_REG_ADC_PGA), 0x1AU, "0x14 should be 0x1A");
	/* ADC ramp. */
	zassert_equal(reg_get(ES8311_REG_ADC_OSR), 0x40U, "0x15 should be 0x40");
	/* ADC control. */
	zassert_equal(reg_get(ES8311_REG_ADC_CTRL), 0x24U, "0x16 should be 0x24");
	/* ADC digital volume ~0 dB. */
	zassert_equal(reg_get(ES8311_REG_ADC_VOLUME), 0xBFU, "0x17 should be 0xBF");
	/* ADC HPF + EQ bypass: cancels the digital DC offset (all Espressif refs). */
	zassert_equal(reg_get(ES8311_REG_ADC_HPF1), 0x0AU, "0x1B should be 0x0A");
	zassert_equal(reg_get(ES8311_REG_ADC_HPF2), 0x6AU, "0x1C should be 0x6A");
	/* 0x44 ADCDAT mux = plain ADC data on ASDOUT (no digital DAC feedback). */
	zassert_equal(reg_get(ES8311_REG_ADC_MUX), 0x08U, "0x44 should be 0x08");
	zassert_equal(reg_get(ES8311_REG_ADC_GP45), 0x00U, "0x45 should be 0x00");

	/* PLAYBACK_CAPTURE must also still emit the DAC path (spot-check). */
	zassert_equal(reg_get(ES8311_REG_SDP_IN), 0x0CU, "0x09 (DAC SDP) should be 0x0C");
	zassert_equal(reg_get(ES8311_REG_SYSTEM_12), 0x00U, "0x12 (DAC power) should be 0x00");
}

/*
 * A capture-only route must be accepted and power the ADC (0x0E) without
 * powering the DAC (0x12 left untouched).
 */
ZTEST(es8311, test_configure_capture_only)
{
	struct audio_codec_cfg cfg;

	reg_put(ES8311_REG_SYSTEM_0E, 0xFF);
	reg_put(ES8311_REG_SYSTEM_12, 0xFF);

	make_cfg_16k_16bit(&cfg);
	cfg.dai_route = AUDIO_ROUTE_CAPTURE;
	zassert_ok(audio_codec_configure(codec, &cfg), "configure(CAPTURE) failed");

	zassert_equal(reg_get(ES8311_REG_SYSTEM_0E), 0x02U, "ADC power 0x0E should be set");
	zassert_equal(reg_get(ES8311_REG_SYSTEM_12), 0xFFU,
		      "capture-only must not touch DAC power 0x12");
}

ZTEST_SUITE(es8311, NULL, NULL, NULL, NULL, NULL);
