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
extern void emul_es8311_pause_before(const struct emul *target, uint8_t reg);
extern int emul_es8311_wait_paused(const struct emul *target, k_timeout_t timeout);
extern void emul_es8311_release(const struct emul *target);

/*
 * ES8311 register map (the subset the driver touches), named after the fields
 * the Everest ES8311 user guide actually puts there: 0x03/0x04 are oversampling
 * rates and 0x05 holds the ADC and DAC clock dividers, not the other way round.
 */
#define ES8311_REG_RESET       0x00
#define ES8311_REG_CLK_MANAGER 0x01
#define ES8311_REG_CLK_PRE     0x02 /* DIV_PRE, MULT_PRE */
#define ES8311_REG_ADC_OSR     0x03 /* ADC_FSMODE, ADC_OSR */
#define ES8311_REG_DAC_OSR     0x04 /* DAC_OSR */
#define ES8311_REG_CLK_DIV     0x05 /* DIV_CLKADC, DIV_CLKDAC */
#define ES8311_REG_CLK_BCLK    0x06
#define ES8311_REG_CLK_LRCK_H  0x07
#define ES8311_REG_CLK_LRCK_L  0x08
#define ES8311_REG_SDP_IN      0x09
#define ES8311_REG_SYSTEM_0D   0x0D
#define ES8311_REG_SYSTEM_12   0x12
#define ES8311_REG_SYSTEM_13   0x13
#define ES8311_REG_DAC_MUTE    0x31
#define ES8311_REG_DAC_VOLUME  0x32
#define ES8311_REG_DAC_EQ      0x37
/* ADC / capture path registers. */
#define ES8311_REG_SDP_OUT     0x0A /* bit 6 is the ADC serial-port mute */
#define ES8311_REG_SYSTEM_0E   0x0E
#define ES8311_REG_ADC_PGA     0x14
#define ES8311_REG_ADC_RAMP    0x15 /* ADC_RAMPRATE, not an OSR */
#define ES8311_REG_ADC_SCALE   0x16 /* ADC polarity, ADC_SCALE */
#define ES8311_REG_ADC_VOLUME  0x17
#define ES8311_REG_ADC_HPF1    0x1B
#define ES8311_REG_ADC_HPF2    0x1C
#define ES8311_REG_ADC_MUX     0x44
#define ES8311_REG_ADC_GP45    0x45
#define ES8311_REG_CHIP_ID1    0xFD
#define ES8311_REG_CHIP_ID2    0xFE

#define ES8311_SDP_MUTE 0x40 /* 0x09 / 0x0A bit 6 */

/* Every rate the driver accepts, and the 256fs master clock each one implies. */
static const uint32_t supported_rates[] = {
	8000U, 11025U, 12000U, 16000U, 22050U, 24000U, 32000U, 44100U, 48000U,
};

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

/* A configuration with the master clock derived from BCLK (mclk_freq == 0). */
static void make_cfg(struct audio_codec_cfg *cfg, uint32_t rate, audio_route_t route)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->mclk_freq = 0U;
	cfg->dai_type = AUDIO_DAI_TYPE_I2S;
	cfg->dai_route = route;
	cfg->dai_cfg.i2s.word_size = AUDIO_PCM_WIDTH_16_BITS;
	cfg->dai_cfg.i2s.channels = 2;
	cfg->dai_cfg.i2s.frame_clk_freq = rate;
}

/* 16 kHz / 16-bit playback, the configuration the application uses. */
static void make_cfg_16k_16bit(struct audio_codec_cfg *cfg)
{
	make_cfg(cfg, AUDIO_PCM_RATE_16K, AUDIO_ROUTE_PLAYBACK);
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
 * success and the device stays ready (warn-and-continue).
 */
ZTEST(es8311, test_init_wrong_chip_id_warns_and_continues)
{
	int ret;

	zassert_true(device_is_ready(codec), "codec device not ready before re-init");

	emul_es8311_set_chip_id(emul, 0x00U, 0x00U);
	zassert_equal(reg_get(ES8311_REG_CHIP_ID1), 0x00U, "chip id1 seed failed");
	zassert_equal(reg_get(ES8311_REG_CHIP_ID2), 0x00U, "chip id2 seed failed");

	/* Force the driver init to run again against the wrong identity. */
	codec->state->initialized = false;
	ret = device_init(codec);

	zassert_ok(ret, "init() must succeed despite wrong chip id (got %d)", ret);
	zassert_true(device_is_ready(codec), "device must stay ready after wrong-id init");

	/* Restore the correct identity for the remaining tests. */
	emul_es8311_set_chip_id(emul, 0x83U, 0x11U);
}

/*
 * configure() for 16 kHz / 16-bit must emit the expected register sequence.
 * Seed every touched register to the opposite of the expected value first, so
 * each assertion requires a real write by the driver.
 */
ZTEST(es8311, test_configure_16k_16bit_sequence)
{
	struct audio_codec_cfg cfg;

	/* Poison the registers the sequence is expected to set. */
	reg_put(ES8311_REG_RESET, 0x00);
	reg_put(ES8311_REG_CLK_MANAGER, 0x00);
	reg_put(ES8311_REG_CLK_PRE, 0x00);
	reg_put(ES8311_REG_CLK_BCLK, 0xFF);
	reg_put(ES8311_REG_CLK_LRCK_L, 0x00);
	reg_put(ES8311_REG_SDP_IN, 0xFF);
	reg_put(ES8311_REG_SDP_OUT, 0x0C);
	reg_put(ES8311_REG_SYSTEM_0D, 0xFF);
	reg_put(ES8311_REG_SYSTEM_0E, 0x02);
	reg_put(ES8311_REG_SYSTEM_12, 0xFF);
	reg_put(ES8311_REG_SYSTEM_13, 0x00);
	reg_put(ES8311_REG_ADC_PGA, 0x1A);
	reg_put(ES8311_REG_DAC_EQ, 0x00);
	reg_put(ES8311_REG_DAC_MUTE, 0xFF);

	make_cfg_16k_16bit(&cfg);
	zassert_ok(audio_codec_configure(codec, &cfg), "configure() failed");

	/* CSM on. */
	zassert_equal(reg_get(ES8311_REG_RESET), 0x80U, "0x00 should be 0x80");
	/* Master clock from BCLK, and the ADC's clocks gated off on a playback route. */
	zassert_equal(reg_get(ES8311_REG_CLK_MANAGER), 0xB5U, "0x01 should be 0xB5");
	/* DIV_PRE = 1, MULT_PRE = x8. */
	zassert_equal(reg_get(ES8311_REG_CLK_PRE), 0x18U, "0x02 should be 0x18");
	/* BCLK_CON clear, so the codec stays the I2S clock slave. */
	zassert_equal(reg_get(ES8311_REG_CLK_BCLK), 0x03U, "0x06 should be 0x03");
	/* DIV_LRCK low byte. */
	zassert_equal(reg_get(ES8311_REG_CLK_LRCK_L), 0xFFU, "0x08 should be 0xFF");
	/* The DAC serial port carries I2S, 16-bit, unmuted. */
	zassert_equal(reg_get(ES8311_REG_SDP_IN), 0x0CU, "0x09 should be 0x0C");
	/* The shared analog block is up, with the ADC's own references down. */
	zassert_equal(reg_get(ES8311_REG_SYSTEM_0D), 0x31U, "0x0D should be 0x31");
	/* DAC power up. */
	zassert_equal(reg_get(ES8311_REG_SYSTEM_12), 0x00U, "0x12 should be 0x00");
	/* Headphone output path. */
	zassert_equal(reg_get(ES8311_REG_SYSTEM_13), 0x10U, "0x13 should be 0x10");
	/* EQ bypass. */
	zassert_equal(reg_get(ES8311_REG_DAC_EQ), 0x08U, "0x37 should be 0x08");
	/* Default DAC volume programmed. */
	zassert_equal(reg_get(ES8311_REG_DAC_VOLUME), 0xC0U, "0x32 should be default 0xC0");
	/* Unmuted at end of configure. */
	zassert_equal(reg_get(ES8311_REG_DAC_MUTE), 0x00U, "0x31 should be unmuted (0x00)");

	/*
	 * A playback-only route owes the caller a microphone that is off, not merely
	 * one whose data is being thrown away. The ADC is powered down, its serial
	 * port is muted, and the microphone is taken off the input mux.
	 */
	zassert_equal(reg_get(ES8311_REG_SYSTEM_0E), 0x62U,
		      "0x0E: the ADC must be powered down on a playback-only route");
	zassert_equal(reg_get(ES8311_REG_ADC_PGA), 0x00U,
		      "0x14: the microphone must be taken off the input mux");
	zassert_equal(reg_get(ES8311_REG_SDP_OUT) & ES8311_SDP_MUTE, ES8311_SDP_MUTE,
		      "0x0A: the ADC serial port must be muted");
}

/*
 * Within configure(), reset (0x00) must precede the clock manager (0x01), and
 * the analog power-up (0x0D) must precede the DAC power-up (0x12).
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

/*
 * configure() with a capture route must additionally emit the ADC register
 * sequence. These values target a differential analog MIC1 at 16 kHz / 16-bit.
 * PLAYBACK_CAPTURE must still emit the DAC sequence too.
 */
ZTEST(es8311, test_configure_capture_sequence)
{
	struct audio_codec_cfg cfg;

	/* Poison the ADC registers the capture path is expected to set. */
	reg_put(ES8311_REG_SDP_OUT, 0xFF);
	reg_put(ES8311_REG_SYSTEM_0E, 0xFF);
	reg_put(ES8311_REG_ADC_PGA, 0x00);
	reg_put(ES8311_REG_ADC_RAMP, 0x00);
	reg_put(ES8311_REG_ADC_SCALE, 0x00);
	reg_put(ES8311_REG_ADC_VOLUME, 0x00);
	reg_put(ES8311_REG_ADC_HPF1, 0xFF);
	reg_put(ES8311_REG_ADC_HPF2, 0x00);
	reg_put(ES8311_REG_ADC_MUX, 0xFF);
	reg_put(ES8311_REG_ADC_GP45, 0xFF);

	make_cfg_16k_16bit(&cfg);
	cfg.dai_route = AUDIO_ROUTE_PLAYBACK_CAPTURE;
	zassert_ok(audio_codec_configure(codec, &cfg), "configure(PLAYBACK_CAPTURE) failed");

	/* ADC serial data port: standard I2S, 16-bit, not muted. */
	zassert_equal(reg_get(ES8311_REG_SDP_OUT), 0x0CU, "0x0A should be 0x0C");
	/* ADC power up. */
	zassert_equal(reg_get(ES8311_REG_SYSTEM_0E), 0x02U, "0x0E should be 0x02");
	/* Differential MIC1 pair (LINSEL = 1) at the 30 dB PGA maximum. */
	zassert_equal(reg_get(ES8311_REG_ADC_PGA), 0x1AU, "0x14 should be 0x1A");
	/* ADC volume ramp rate. */
	zassert_equal(reg_get(ES8311_REG_ADC_RAMP), 0x40U, "0x15 should be 0x40");
	/* ADC digital scale. */
	zassert_equal(reg_get(ES8311_REG_ADC_SCALE), 0x24U, "0x16 should be 0x24");
	/* ADC digital volume, 0 dB by default. */
	zassert_equal(reg_get(ES8311_REG_ADC_VOLUME), 0xBFU, "0x17 should be 0xBF");
	/* ADC HPF + EQ bypass: cancels the digital DC offset. */
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
 * A capture-only route powers the ADC up and the DAC DOWN.
 *
 * The previous version of this test asserted that 0x12 was left untouched, which
 * did not describe correct behaviour: it described the bug. Register 0x00 does
 * not reset the register file, so a DAC a previous PLAYBACK_CAPTURE route powered
 * up stays powered, and the speaker path stays live behind a caller that asked
 * for capture only.
 */
ZTEST(es8311, test_configure_capture_only)
{
	struct audio_codec_cfg cfg;

	/* Come from a route that had the DAC up, which is the case that used to leak. */
	make_cfg(&cfg, AUDIO_PCM_RATE_16K, AUDIO_ROUTE_PLAYBACK_CAPTURE);
	zassert_ok(audio_codec_configure(codec, &cfg), "configure(PLAYBACK_CAPTURE) failed");
	zassert_equal(reg_get(ES8311_REG_SYSTEM_12), 0x00U, "the DAC should be up here");

	make_cfg(&cfg, AUDIO_PCM_RATE_16K, AUDIO_ROUTE_CAPTURE);
	zassert_ok(audio_codec_configure(codec, &cfg), "configure(CAPTURE) failed");

	zassert_equal(reg_get(ES8311_REG_SYSTEM_0E), 0x02U, "0x0E: the ADC must be powered up");
	zassert_equal(reg_get(ES8311_REG_SYSTEM_12), 0x02U,
		      "0x12: the DAC must be powered DOWN, not left as the previous route "
		      "left it");
	zassert_equal(reg_get(ES8311_REG_CLK_MANAGER), 0xBAU,
		      "0x01: the DAC clocks must be gated off");
	zassert_equal(reg_get(ES8311_REG_SYSTEM_0D), 0x09U,
		      "0x0D: the DAC's own reference must be dropped");
	zassert_equal(reg_get(ES8311_REG_SDP_IN) & ES8311_SDP_MUTE, ES8311_SDP_MUTE,
		      "0x09: the DAC serial port must be muted");
}

/*
 * The other direction of the same defect: going from PLAYBACK_CAPTURE to
 * PLAYBACK must take the microphone down, not just stop reading it. A muted
 * serial port would leave the PGA and the modulator running on a live mic.
 */
ZTEST(es8311, test_route_transition_drops_the_microphone)
{
	struct audio_codec_cfg cfg;

	make_cfg(&cfg, AUDIO_PCM_RATE_16K, AUDIO_ROUTE_PLAYBACK_CAPTURE);
	zassert_ok(audio_codec_configure(codec, &cfg), "configure(PLAYBACK_CAPTURE) failed");
	zassert_equal(reg_get(ES8311_REG_SYSTEM_0E), 0x02U, "the ADC should be up here");
	zassert_equal(reg_get(ES8311_REG_ADC_PGA), 0x1AU, "the mic should be on the mux here");

	make_cfg(&cfg, AUDIO_PCM_RATE_16K, AUDIO_ROUTE_PLAYBACK);
	zassert_ok(audio_codec_configure(codec, &cfg), "configure(PLAYBACK) failed");

	zassert_equal(reg_get(ES8311_REG_SYSTEM_0E), 0x62U,
		      "0x0E: the ADC must be powered down, not left running");
	zassert_equal(reg_get(ES8311_REG_ADC_PGA), 0x00U,
		      "0x14: the microphone must be taken off the input mux");
	zassert_equal(reg_get(ES8311_REG_CLK_MANAGER), 0xB5U,
		      "0x01: the ADC clocks must be gated off");
	zassert_equal(reg_get(ES8311_REG_SYSTEM_0D), 0x31U,
		      "0x0D: the ADC's own bias and reference must be dropped");
	zassert_equal(reg_get(ES8311_REG_SDP_OUT) & ES8311_SDP_MUTE, ES8311_SDP_MUTE,
		      "0x0A: the ADC serial port must be muted");
	zassert_equal(reg_get(ES8311_REG_SYSTEM_12), 0x00U, "the DAC must still be up");
}

/*
 * And the properties must respect the route too. On a playback-only route,
 * apply_properties() must not clear the microphone's serial-port mute that
 * configure() just set: the cached input state is not a licence to re-open a
 * path the route took down.
 */
ZTEST(es8311, test_apply_properties_respects_the_route)
{
	struct audio_codec_cfg cfg;
	audio_property_value_t unmute = {.mute = false};

	make_cfg(&cfg, AUDIO_PCM_RATE_16K, AUDIO_ROUTE_PLAYBACK);
	zassert_ok(audio_codec_configure(codec, &cfg), "configure(PLAYBACK) failed");
	zassert_equal(reg_get(ES8311_REG_SDP_OUT) & ES8311_SDP_MUTE, ES8311_SDP_MUTE,
		      "the ADC port should be muted after a playback-only configure");

	/* The default cached input state is "not muted". Applying it must not win. */
	zassert_ok(audio_codec_set_property(codec, AUDIO_PROPERTY_INPUT_MUTE,
					    AUDIO_CHANNEL_ALL, unmute),
		   "set INPUT_MUTE(false) failed");
	zassert_ok(audio_codec_apply_properties(codec), "apply_properties failed");

	zassert_equal(reg_get(ES8311_REG_SDP_OUT) & ES8311_SDP_MUTE, ES8311_SDP_MUTE,
		      "apply_properties() must not re-open the microphone on a route that "
		      "does not carry capture");
}

/*
 * The clock tree is ratiometric: the master clock is 8 * BCLK, and a 16-bit
 * stereo frame carries 32 bit clocks, so the master clock is 256 * Fs at every
 * supported rate. That means one register set must serve them all. Configure at
 * each supported rate and assert the clock registers are identical every time.
 */
ZTEST(es8311, test_configure_all_supported_rates)
{
	struct audio_codec_cfg cfg;

	for (size_t i = 0; i < ARRAY_SIZE(supported_rates); i++) {
		uint32_t rate = supported_rates[i];

		/* Poison the clock registers before each rate. */
		reg_put(ES8311_REG_CLK_MANAGER, 0x00);
		reg_put(ES8311_REG_CLK_PRE, 0x00);
		reg_put(ES8311_REG_ADC_OSR, 0x00);
		reg_put(ES8311_REG_DAC_OSR, 0x00);
		reg_put(ES8311_REG_CLK_DIV, 0xFF);
		reg_put(ES8311_REG_CLK_BCLK, 0x00);
		reg_put(ES8311_REG_CLK_LRCK_H, 0xFF);
		reg_put(ES8311_REG_CLK_LRCK_L, 0x00);

		make_cfg(&cfg, rate, AUDIO_ROUTE_PLAYBACK);
		zassert_ok(audio_codec_configure(codec, &cfg), "configure(%u Hz) failed", rate);

		/* A playback-only route: the ADC's clocks are gated off. */
		zassert_equal(reg_get(ES8311_REG_CLK_MANAGER), 0xB5U, "0x01 at %u Hz", rate);
		zassert_equal(reg_get(ES8311_REG_CLK_PRE), 0x18U, "0x02 at %u Hz", rate);
		zassert_equal(reg_get(ES8311_REG_ADC_OSR), 0x10U, "0x03 at %u Hz", rate);
		zassert_equal(reg_get(ES8311_REG_DAC_OSR), 0x10U, "0x04 at %u Hz", rate);
		zassert_equal(reg_get(ES8311_REG_CLK_DIV), 0x00U, "0x05 at %u Hz", rate);
		zassert_equal(reg_get(ES8311_REG_CLK_BCLK), 0x03U, "0x06 at %u Hz", rate);
		zassert_equal(reg_get(ES8311_REG_CLK_LRCK_H), 0x00U, "0x07 at %u Hz", rate);
		zassert_equal(reg_get(ES8311_REG_CLK_LRCK_L), 0xFFU, "0x08 at %u Hz", rate);
	}
}

/* Rates outside the supported set must be rejected, not silently mis-clocked. */
ZTEST(es8311, test_configure_rejects_bad_rates)
{
	static const uint32_t bad[] = {0U, 7999U, 44099U, 96000U, 192000U};
	struct audio_codec_cfg cfg;

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		make_cfg(&cfg, bad[i], AUDIO_ROUTE_PLAYBACK);
		zassert_equal(audio_codec_configure(codec, &cfg), -ENOTSUP,
			      "rate %u must be rejected", bad[i]);
	}
}

/*
 * The master clock is derived from BCLK, so the frame has to carry 32 bit
 * clocks for the clock tree to land on 256fs. A 24-bit frame carries 48 and a
 * 32-bit frame 64, which would silently shift the pitch: both must be rejected.
 */
ZTEST(es8311, test_configure_rejects_non_16bit_word)
{
	struct audio_codec_cfg cfg;

	make_cfg(&cfg, AUDIO_PCM_RATE_16K, AUDIO_ROUTE_PLAYBACK);
	cfg.dai_cfg.i2s.word_size = AUDIO_PCM_WIDTH_24_BITS;
	zassert_equal(audio_codec_configure(codec, &cfg), -ENOTSUP,
		      "24-bit words must be rejected (BCLK-derived MCLK needs 16-bit)");

	make_cfg(&cfg, AUDIO_PCM_RATE_16K, AUDIO_ROUTE_PLAYBACK);
	cfg.dai_cfg.i2s.word_size = AUDIO_PCM_WIDTH_32_BITS;
	zassert_equal(audio_codec_configure(codec, &cfg), -ENOTSUP,
		      "32-bit words must be rejected (BCLK-derived MCLK needs 16-bit)");
}

/*
 * audio_codec_cfg.mclk_freq describes the clock fed to the codec's MCLK *input*
 * pin. This driver never uses that pin: it derives the master clock from BCLK.
 * So zero is the only meaningful value, and anything else must be rejected
 * rather than silently ignored. In particular the internal 256fs figure is NOT
 * an MCLK input frequency, and accepting it would be describing a clock that is
 * not on any pin.
 */
ZTEST(es8311, test_configure_mclk_must_be_zero)
{
	struct audio_codec_cfg cfg;

	for (size_t i = 0; i < ARRAY_SIZE(supported_rates); i++) {
		uint32_t rate = supported_rates[i];

		make_cfg(&cfg, rate, AUDIO_ROUTE_PLAYBACK);
		cfg.mclk_freq = 0U;
		zassert_ok(audio_codec_configure(codec, &cfg),
			   "mclk 0 (BCLK-derived) must be accepted at %u Hz", rate);

		make_cfg(&cfg, rate, AUDIO_ROUTE_PLAYBACK);
		cfg.mclk_freq = rate * 256U;
		zassert_equal(audio_codec_configure(codec, &cfg), -ENOTSUP,
			      "the internal 256fs clock is not an MCLK input and must be "
			      "rejected at %u Hz",
			      rate);

		make_cfg(&cfg, rate, AUDIO_ROUTE_PLAYBACK);
		cfg.mclk_freq = 12288000U;
		zassert_equal(audio_codec_configure(codec, &cfg), -ENOTSUP,
			      "an external MCLK is not supported and must be rejected at %u Hz",
			      rate);
	}
}

/* configure() must reject a non-I2S DAI, an unsupported route and a bad format. */
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
	cfg.dai_cfg.i2s.format = I2S_FMT_DATA_FORMAT_LEFT_JUSTIFIED;
	zassert_equal(audio_codec_configure(codec, &cfg), -ENOTSUP,
		      "left-justified data format must be rejected");

	make_cfg_16k_16bit(&cfg);
	cfg.dai_cfg.i2s.format = I2S_FMT_DATA_ORDER_LSB;
	zassert_equal(audio_codec_configure(codec, &cfg), -ENOTSUP,
		      "LSB-first data ordering must be rejected");

	make_cfg_16k_16bit(&cfg);
	cfg.dai_cfg.i2s.format = I2S_FMT_BIT_CLK_INV;
	zassert_equal(audio_codec_configure(codec, &cfg), -ENOTSUP,
		      "inverted bit clock must be rejected");
}

/*
 * OUTPUT_VOLUME set_property + apply_properties must write the DAC volume
 * register (0x32). 0 dB maps to code 0xBF.
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

/* The dB-to-code mapping must clamp at both ends of the register range. */
ZTEST(es8311, test_volume_clamps)
{
	audio_property_value_t val;

	val.vol = 1000;
	zassert_ok(audio_codec_set_property(codec, AUDIO_PROPERTY_OUTPUT_VOLUME,
					    AUDIO_CHANNEL_ALL, val),
		   "set OUTPUT_VOLUME(+1000 dB) failed");
	zassert_ok(audio_codec_apply_properties(codec), "apply_properties failed");
	zassert_equal(reg_get(ES8311_REG_DAC_VOLUME), 0xFFU,
		      "an absurdly high volume must clamp to the 0xFF maximum");

	val.vol = -1000;
	zassert_ok(audio_codec_set_property(codec, AUDIO_PROPERTY_OUTPUT_VOLUME,
					    AUDIO_CHANNEL_ALL, val),
		   "set OUTPUT_VOLUME(-1000 dB) failed");
	zassert_ok(audio_codec_apply_properties(codec), "apply_properties failed");
	zassert_equal(reg_get(ES8311_REG_DAC_VOLUME), 0x01U,
		      "an absurdly low volume must clamp to the -95 dB code");

	/* Leave 0 dB behind so later tests see a known DAC volume. */
	val.vol = 0;
	zassert_ok(audio_codec_set_property(codec, AUDIO_PROPERTY_OUTPUT_VOLUME,
					    AUDIO_CHANNEL_ALL, val),
		   "restore OUTPUT_VOLUME failed");
	zassert_ok(audio_codec_apply_properties(codec), "apply_properties failed");
}

/*
 * OUTPUT_MUTE set_property + apply_properties must write the DAC mute field
 * (0x31 bits [6:5]).
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

/* start_output() and stop_output() must drive the DAC mute field directly. */
ZTEST(es8311, test_start_stop_output)
{
	reg_put(ES8311_REG_DAC_MUTE, 0x60);
	audio_codec_start_output(codec);
	zassert_equal(reg_get(ES8311_REG_DAC_MUTE) & 0x60U, 0x00U,
		      "start_output() must unmute the DAC");

	audio_codec_stop_output(codec);
	zassert_equal(reg_get(ES8311_REG_DAC_MUTE) & 0x60U, 0x60U,
		      "stop_output() must mute the DAC");

	audio_codec_start_output(codec);
	zassert_equal(reg_get(ES8311_REG_DAC_MUTE) & 0x60U, 0x00U,
		      "start_output() must unmute again");
}

/*
 * stop_output() caches the muted state, and a later configure() has to re-apply
 * it: otherwise reconfiguring a stopped stream would silently start the speaker.
 */
ZTEST(es8311, test_stop_output_state_survives_configure)
{
	struct audio_codec_cfg cfg;

	audio_codec_stop_output(codec);
	reg_put(ES8311_REG_DAC_MUTE, 0x00);

	make_cfg_16k_16bit(&cfg);
	zassert_ok(audio_codec_configure(codec, &cfg), "configure() failed");
	zassert_equal(reg_get(ES8311_REG_DAC_MUTE) & 0x60U, 0x60U,
		      "configure() must re-apply the cached mute state, not unmute");

	/* Leave the DAC unmuted for the remaining tests. */
	audio_codec_start_output(codec);
}

/*
 * INPUT_VOLUME must reach the ADC digital volume register (0x17), using the same
 * linear mapping as the DAC: 0 dB is 0xBF and each dB is two codes.
 */
ZTEST(es8311, test_set_input_volume)
{
	audio_property_value_t val;

	reg_put(ES8311_REG_ADC_VOLUME, 0x00);

	val.vol = 0;
	zassert_ok(audio_codec_set_property(codec, AUDIO_PROPERTY_INPUT_VOLUME,
					    AUDIO_CHANNEL_ALL, val),
		   "set INPUT_VOLUME(0 dB) failed");
	zassert_ok(audio_codec_apply_properties(codec), "apply_properties failed");
	zassert_equal(reg_get(ES8311_REG_ADC_VOLUME), 0xBFU, "0 dB should map to 0xBF");

	val.vol = -6;
	zassert_ok(audio_codec_set_property(codec, AUDIO_PROPERTY_INPUT_VOLUME,
					    AUDIO_CHANNEL_ALL, val),
		   "set INPUT_VOLUME(-6 dB) failed");
	zassert_ok(audio_codec_apply_properties(codec), "apply_properties failed");
	zassert_equal(reg_get(ES8311_REG_ADC_VOLUME), 0xB3U, "-6 dB should map to 0xB3");

	/* Restore 0 dB so the configure() tests see the default ADC volume. */
	val.vol = 0;
	zassert_ok(audio_codec_set_property(codec, AUDIO_PROPERTY_INPUT_VOLUME,
					    AUDIO_CHANNEL_ALL, val),
		   "restore INPUT_VOLUME failed");
	zassert_ok(audio_codec_apply_properties(codec), "apply_properties failed");
}

/*
 * INPUT_MUTE must use the ADC serial data port's own mute bit (0x0A bit 6), not
 * the volume: the ADC volume register bottoms out at -95.5 dB, which is quiet
 * but is not a mute, and collapsing the volume would also destroy the level the
 * caller set.
 */
ZTEST(es8311, test_set_input_mute)
{
	audio_property_value_t mute = {.mute = true};
	audio_property_value_t unmute = {.mute = false};
	audio_property_value_t vol;

	/* Park the input volume somewhere recognisable first. */
	vol.vol = 6;
	zassert_ok(audio_codec_set_property(codec, AUDIO_PROPERTY_INPUT_VOLUME,
					    AUDIO_CHANNEL_ALL, vol),
		   "set INPUT_VOLUME(+6 dB) failed");
	zassert_ok(audio_codec_apply_properties(codec), "apply_properties failed");
	zassert_equal(reg_get(ES8311_REG_ADC_VOLUME), 0xCBU, "+6 dB should map to 0xCB");

	reg_put(ES8311_REG_SDP_OUT, 0x0C);
	zassert_ok(audio_codec_set_property(codec, AUDIO_PROPERTY_INPUT_MUTE,
					    AUDIO_CHANNEL_ALL, mute),
		   "set INPUT_MUTE(true) failed");
	zassert_ok(audio_codec_apply_properties(codec), "apply_properties failed");
	zassert_equal(reg_get(ES8311_REG_SDP_OUT) & ES8311_SDP_MUTE, ES8311_SDP_MUTE,
		      "an input mute must set the ADC serial port mute bit (0x0A bit 6)");
	zassert_equal(reg_get(ES8311_REG_ADC_VOLUME), 0xCBU,
		      "muting the input must not disturb the input volume");

	zassert_ok(audio_codec_set_property(codec, AUDIO_PROPERTY_INPUT_MUTE,
					    AUDIO_CHANNEL_ALL, unmute),
		   "set INPUT_MUTE(false) failed");
	zassert_ok(audio_codec_apply_properties(codec), "apply_properties failed");
	zassert_equal(reg_get(ES8311_REG_SDP_OUT) & ES8311_SDP_MUTE, 0x00U,
		      "unmuting must clear the ADC serial port mute bit");
	zassert_equal(reg_get(ES8311_REG_ADC_VOLUME), 0xCBU,
		      "unmuting must leave the input volume where the caller put it");

	/* Restore 0 dB for the remaining tests. */
	vol.vol = 0;
	zassert_ok(audio_codec_set_property(codec, AUDIO_PROPERTY_INPUT_VOLUME,
					    AUDIO_CHANNEL_ALL, vol),
		   "restore INPUT_VOLUME failed");
	zassert_ok(audio_codec_apply_properties(codec), "apply_properties failed");
}

/*
 * A cached input mute must survive into the ADC sequence configure() emits, the
 * same way the output mute does: reconfiguring a muted input must not silently
 * open the microphone.
 */
ZTEST(es8311, test_input_mute_survives_configure)
{
	struct audio_codec_cfg cfg;
	audio_property_value_t mute = {.mute = true};
	audio_property_value_t unmute = {.mute = false};

	zassert_ok(audio_codec_set_property(codec, AUDIO_PROPERTY_INPUT_MUTE,
					    AUDIO_CHANNEL_ALL, mute),
		   "set INPUT_MUTE(true) failed");

	reg_put(ES8311_REG_SDP_OUT, 0x00);

	make_cfg(&cfg, AUDIO_PCM_RATE_16K, AUDIO_ROUTE_CAPTURE);
	zassert_ok(audio_codec_configure(codec, &cfg), "configure(CAPTURE) failed");

	zassert_equal(reg_get(ES8311_REG_SDP_OUT), 0x0CU | ES8311_SDP_MUTE,
		      "configure() must re-apply the cached input mute");

	/* Unmute for the remaining tests. */
	zassert_ok(audio_codec_set_property(codec, AUDIO_PROPERTY_INPUT_MUTE,
					    AUDIO_CHANNEL_ALL, unmute),
		   "restore INPUT_MUTE failed");
	zassert_ok(audio_codec_apply_properties(codec), "apply_properties failed");
}

/*
 * A value set with INPUT_VOLUME before configure() must survive into the ADC
 * sequence that configure() emits, rather than being overwritten by the default.
 */
ZTEST(es8311, test_input_volume_survives_configure)
{
	struct audio_codec_cfg cfg;
	audio_property_value_t vol;

	vol.vol = -12;
	zassert_ok(audio_codec_set_property(codec, AUDIO_PROPERTY_INPUT_VOLUME,
					    AUDIO_CHANNEL_ALL, vol),
		   "set INPUT_VOLUME(-12 dB) failed");

	reg_put(ES8311_REG_ADC_VOLUME, 0x00);

	make_cfg(&cfg, AUDIO_PCM_RATE_16K, AUDIO_ROUTE_CAPTURE);
	zassert_ok(audio_codec_configure(codec, &cfg), "configure(CAPTURE) failed");

	zassert_equal(reg_get(ES8311_REG_ADC_VOLUME), 0xA7U,
		      "configure() must program the cached input volume (-12 dB = 0xA7)");

	/* Restore 0 dB for the remaining tests. */
	vol.vol = 0;
	zassert_ok(audio_codec_set_property(codec, AUDIO_PROPERTY_INPUT_VOLUME,
					    AUDIO_CHANNEL_ALL, vol),
		   "restore INPUT_VOLUME failed");
	zassert_ok(audio_codec_apply_properties(codec), "apply_properties failed");
}

/* A property the codec does not model must be rejected with -ENOTSUP. */
ZTEST(es8311, test_set_property_unsupported)
{
	audio_property_value_t val = {.vol = 0};

	zassert_equal(audio_codec_set_property(codec, (audio_property_t)0x7F,
					       AUDIO_CHANNEL_ALL, val),
		      -ENOTSUP, "an unknown property must be rejected");
}

/* A channel the mono codec cannot address must be rejected with -EINVAL. */
ZTEST(es8311, test_set_property_invalid_channel)
{
	audio_property_value_t val = {.vol = 0};

	zassert_equal(audio_codec_set_property(codec, AUDIO_PROPERTY_OUTPUT_VOLUME,
					       AUDIO_CHANNEL_REAR_LEFT, val),
		      -EINVAL, "a channel the codec has no output for must be rejected");
}

/* An I2C failure during configure() must propagate as an error, and recover. */
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

/* An I2C failure while applying properties must propagate too. */
ZTEST(es8311, test_apply_properties_propagates_i2c_error)
{
	emul_es8311_set_fail(emul, 1);
	zassert_true(audio_codec_apply_properties(codec) < 0,
		     "apply_properties() must return an error on I2C failure");

	emul_es8311_set_fail(emul, 0);
	zassert_ok(audio_codec_apply_properties(codec), "apply_properties() should recover");
}

/*
 * The concurrency test. Everything above runs on one thread, so none of it can
 * see the defect this one is here for.
 *
 * apply_properties() must hold the codec lock across its register writes, not
 * merely across the read of the cached state. If it snapshots and unlocks,
 * stop_output() can slip into the gap, cache "muted" and mute the DAC in
 * hardware, and then have that write overwritten by the value apply_properties()
 * sampled before the gap. The driver's cache would say muted while the speaker
 * stayed live, which is a speaker-safety bug rather than a tidiness one.
 *
 * The emulator parks apply_properties() inside its first register write (the DAC
 * volume at 0x32, which stop_output() never touches), still holding whatever
 * lock the driver holds. A second thread then calls stop_output(). With the lock
 * held across the writes it blocks there and wins afterwards; with the lock
 * released early it runs immediately and loses.
 */
static K_THREAD_STACK_DEFINE(apply_stack, 2048);
static K_THREAD_STACK_DEFINE(stop_stack, 2048);
static struct k_thread apply_thread;
static struct k_thread stop_thread;

static void apply_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	(void)audio_codec_apply_properties(codec);
}

static void stop_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	audio_codec_stop_output(codec);
}

ZTEST(es8311, test_apply_properties_holds_the_lock_across_its_writes)
{
	audio_property_value_t unmute = {.mute = false};
	k_tid_t a;
	k_tid_t b;

	/* Cache "unmuted", and leave the hardware muted so a stale write shows. */
	zassert_ok(audio_codec_set_property(codec, AUDIO_PROPERTY_OUTPUT_MUTE,
					    AUDIO_CHANNEL_ALL, unmute),
		   "set OUTPUT_MUTE(false) failed");
	reg_put(ES8311_REG_DAC_MUTE, 0x60);

	emul_es8311_pause_before(emul, ES8311_REG_DAC_VOLUME);

	a = k_thread_create(&apply_thread, apply_stack, K_THREAD_STACK_SIZEOF(apply_stack),
			    apply_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(2), 0, K_NO_WAIT);
	zassert_ok(emul_es8311_wait_paused(emul, K_SECONDS(1)),
		   "apply_properties() never reached the DAC volume write");

	b = k_thread_create(&stop_thread, stop_stack, K_THREAD_STACK_SIZEOF(stop_stack),
			    stop_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(2), 0, K_NO_WAIT);

	/* Give stop_output() time either to block on the lock or to race past it. */
	k_msleep(50);

	emul_es8311_release(emul);
	zassert_ok(k_thread_join(a, K_SECONDS(1)), "the apply_properties() thread hung");
	zassert_ok(k_thread_join(b, K_SECONDS(1)), "the stop_output() thread hung");

	zassert_equal(reg_get(ES8311_REG_DAC_MUTE) & 0x60U, 0x60U,
		      "stop_output() must win: the speaker must not be left live while the "
		      "driver's cached state says muted");

	/* Leave the DAC unmuted for anything that runs after this. */
	audio_codec_start_output(codec);
}

/*
 * Every test starts from a route that carries both directions.
 *
 * The driver now refuses to touch a converter the current route does not carry,
 * which is the whole point of the route-transition fix, so a test that wants to
 * exercise the DAC or the ADC has to have asked for that direction. Without this
 * the property tests would silently become no-ops after any test that left a
 * single-direction route behind, and they would pass by doing nothing.
 */
static void es8311_before(void *fixture)
{
	struct audio_codec_cfg cfg;

	ARG_UNUSED(fixture);

	make_cfg(&cfg, AUDIO_PCM_RATE_16K, AUDIO_ROUTE_PLAYBACK_CAPTURE);
	(void)audio_codec_configure(codec, &cfg);
}

ZTEST_SUITE(es8311, NULL, NULL, es8311_before, NULL, NULL);
