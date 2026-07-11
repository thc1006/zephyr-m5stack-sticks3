/*
 * Copyright (c) 2026 Hsiu-Chi Tsai
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Everest Semiconductor ES8311 mono audio codec.
 *
 * Control interface: I2C (7-bit address 0x18 or 0x19, selected by the CE pin).
 * Audio interface: I2S / PCM, with the codec as the clock slave: the SoC drives
 * both the bit clock and the frame clock.
 *
 * The codec takes its internal master clock from the I2S bit clock (register
 * 0x01 bit 7), so no dedicated MCLK line is required. Register programming
 * follows the Everest ES8311 user guide (rev 1.11). The playback and capture
 * paths are validated on hardware.
 */

#define DT_DRV_COMPAT everest_es8311

#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define LOG_LEVEL CONFIG_AUDIO_CODEC_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(es8311);

/* Register map (the subset this driver touches). */
#define ES8311_REG_RESET       0x00U /* reset / clock state machine */
#define ES8311_REG_CLK_MANAGER 0x01U /* master clock source + clock enables */
#define ES8311_REG_CLK_PRE     0x02U /* DIV_PRE, MULT_PRE */
#define ES8311_REG_ADC_OSR     0x03U /* ADC_FSMODE, ADC_OSR */
#define ES8311_REG_DAC_OSR     0x04U /* DAC_OSR */
#define ES8311_REG_CLK_DIV     0x05U /* DIV_CLKADC, DIV_CLKDAC */
#define ES8311_REG_CLK_BCLK    0x06U /* BCLK_CON, BCLK_INV, DIV_BCLK */
#define ES8311_REG_CLK_LRCK_H  0x07U /* tri-state controls, DIV_LRCK[11:8] */
#define ES8311_REG_CLK_LRCK_L  0x08U /* DIV_LRCK[7:0] */
#define ES8311_REG_SDP_IN      0x09U /* serial data port, DAC path (SDIN) */
#define ES8311_REG_SDP_OUT     0x0AU /* serial data port, ADC path (ASDOUT) */
#define ES8311_REG_SYSTEM_0D   0x0DU /* analog power: bias, VREF, VMID */
#define ES8311_REG_SYSTEM_0E   0x0EU /* ADC power */
#define ES8311_REG_SYSTEM_12   0x12U /* DAC power */
#define ES8311_REG_SYSTEM_13   0x13U /* output select: line-out or headphone */
#define ES8311_REG_ADC_PGA     0x14U /* LINSEL microphone mux, PGA gain */
#define ES8311_REG_ADC_RAMP    0x15U /* ADC volume ramp rate */
#define ES8311_REG_ADC_SCALE   0x16U /* ADC polarity, ADC_SCALE */
#define ES8311_REG_ADC_VOLUME  0x17U /* ADC digital volume */
#define ES8311_REG_ADC_HPF1    0x1BU /* ADC high-pass filter, stage 1 */
#define ES8311_REG_ADC_HPF2    0x1CU /* EQ bypass + ADC high-pass filter stage 2 */
#define ES8311_REG_DAC_MUTE    0x31U /* DAC mute */
#define ES8311_REG_DAC_VOLUME  0x32U /* DAC digital volume */
#define ES8311_REG_DAC_EQ      0x37U /* DAC equaliser bypass */
#define ES8311_REG_GPIO        0x44U /* GPIO, ADCDAT_SEL */
#define ES8311_REG_ADC_GP45    0x45U /* GP control */
#define ES8311_REG_CHIP_ID1    0xFDU /* chip id, high byte */
#define ES8311_REG_CHIP_ID2    0xFEU /* chip id, low byte */

#define ES8311_CHIP_ID1 0x83U
#define ES8311_CHIP_ID2 0x11U

/* 0x00: clock state machine powered up, reset released. */
#define ES8311_RESET_CSM_ON 0x80U

/*
 * 0x01: MCLK_SEL (bit 7) takes the internal master clock from BCLK instead of
 * the MCLK pin, and the remaining bits enable the clock tree (MCLK, BCLK, and
 * the digital and analog ADC and DAC clocks).
 */
#define ES8311_CLK_MGR_FROM_BCLK 0xBFU

/*
 * Clock tree.
 *
 * The internal master clock is BCLK scaled by DIV_PRE and MULT_PRE (0x02). A
 * standard 16-bit stereo frame carries 32 bit clocks, so BCLK is 32 * Fs, and
 * with DIV_PRE = 1 and MULT_PRE = x8 the internal master clock lands on
 * 256 * Fs. Every divider below is a ratio of that clock, never an absolute
 * frequency, so the same values are correct at every sample rate that preserves
 * the 256fs relationship. Only a 16-bit word does; 24-bit and 32-bit frames
 * carry 48 and 64 bit clocks, which would give 384fs and 512fs, and are
 * rejected in es8311_configure().
 *
 * The user guide requires the ratio of the internal ADC (and DAC) clock to LRCK
 * to be at least 256 and an integral multiple of 16 in single-speed mode. With
 * DIV_CLKADC = DIV_CLKDAC = 1 the ratio is exactly 256 = 16 * 16, and the
 * oversampling rates follow as ratio / 16 = 16.
 */
#define ES8311_CLK_PRE_DIV1_MULT8 0x18U /* 0x02: DIV_PRE = 1, MULT_PRE = x8 */
#define ES8311_ADC_OSR_SINGLE_16  0x10U /* 0x03: single speed, ADC_OSR = 16 */
#define ES8311_DAC_OSR_16         0x10U /* 0x04: DAC_OSR = 16 */
#define ES8311_CLK_DIV_ADC1_DAC1  0x00U /* 0x05: DIV_CLKADC = 1, DIV_CLKDAC = 1 */

/*
 * 0x06 / 0x07 / 0x08 hold the BCLK and LRCK dividers, which the user guide says
 * are inactive in slave mode: the codec detects the master clock to LRCK ratio
 * by itself. They are still written, because two neighbouring fields are not
 * dividers and do matter here: BCLK_CON in 0x06 has to stay clear so the codec
 * does not try to drive the bit clock, and the tri-state controls in 0x07 have
 * to stay clear so the codec actually drives ADCDAT. The divider fields are set
 * to the value the 256fs relationship implies, which would also be correct if
 * the codec were ever used as the clock master.
 */
#define ES8311_BCLK_SLAVE_DIV4 0x03U /* 0x06: BCLK_CON clear, DIV_BCLK = 4 */
#define ES8311_LRCK_DIV_H      0x00U /* 0x07: no tri-state, DIV_LRCK[11:8] = 0 */
#define ES8311_LRCK_DIV_L      0xFFU /* 0x08: DIV_LRCK[7:0], so LRCK = MCLK / 256 */

/*
 * 0x09 and 0x0A, the serial data ports feeding the DAC and coming out of the
 * ADC: word length in bits [4:2] (0b011 selects 16-bit), I2S format in bits
 * [1:0], and a mute bit at bit 6.
 */
#define ES8311_SDP_I2S_16BIT 0x0CU
#define ES8311_SDP_MUTE      0x40U

/* Power sequencing. */
#define ES8311_ANALOG_PWR_ON 0x01U /* 0x0D: bias, VREF and VMID powered */
#define ES8311_ADC_PWR_ON    0x02U /* 0x0E: ADC PGA and modulator powered */
#define ES8311_DAC_PWR_ON    0x00U /* 0x12: DAC powered */
#define ES8311_OUT_HEADPHONE 0x10U /* 0x13: headphone output path */
#define ES8311_DAC_EQ_BYPASS 0x08U /* 0x37: bypass the DAC equaliser */

/*
 * 0x31: the DAC has two mute points, DSMMUTE at bit 6 and DEMMUTE at bit 5. The
 * driver asserts both, as do the vendor reference drivers.
 */
#define ES8311_DAC_MUTE_MASK 0x60U
#define ES8311_DAC_MUTE_ON   0x60U
#define ES8311_DAC_MUTE_OFF  0x00U

/* Settle delays (ms). */
#define ES8311_RESET_DELAY_MS  10
#define ES8311_PWR_UP_DELAY_MS 10
#define ES8311_GPIO_DELAY_MS   1

/*
 * The DAC (0x32) and the ADC (0x17) digital volume registers share one linear
 * layout: 0x00 is the -95.5 dB minimum, 0xBF is 0 dB and 0xFF is the +32 dB
 * maximum, in 0.5 dB steps. The codec API expresses volume in whole dB, so the
 * same conversion serves both, and the reachable range is -95 dB to +32 dB.
 */
#define ES8311_VOL_DB_MAX       32
#define ES8311_VOL_DB_MIN       (-95)
#define ES8311_VOL_0DB_CODE     0xBFU
#define ES8311_VOL_DEFAULT_CODE 0xC0U /* ~ +0.5 dB */

/*
 * Analog capture front end. The ES8311 has a single fully differential
 * microphone input, so 0x14 selects the MIC1P/MIC1N pair (LINSEL = 1) and sets
 * the PGA to its 30 dB maximum, which may need lowering if the captured signal
 * clips.
 */
#define ES8311_ADC_PGA_MIC1_30DB 0x1AU /* 0x14 */
#define ES8311_ADC_RAMP_RATE     0x40U /* 0x15: volume ramp rate */
#define ES8311_ADC_HPF1_VAL      0x0AU /* 0x1B */
#define ES8311_ADC_HPF2_DCBLOCK  0x6AU /* 0x1C: EQ bypass, cancels the DC offset */
#define ES8311_ADC_GP45_DEFAULT  0x00U /* 0x45 */

/*
 * 0x16 selects the ADC digital scale in bits [2:0]; 4 is the +24 dB reset
 * default, which is what this driver keeps. Bit 5 does not appear in the user
 * guide's register table; the vendor reference drivers set it, and this is the
 * sequence that is validated on hardware, so it is set here too.
 */
#define ES8311_ADC_SCALE_24DB 0x24U

/*
 * 0x44 is the GPIO and ADCDAT mux register, not an analog input select.
 * ADCDAT_SEL lives in bits [6:4]; leaving it at 0 puts plain ADC data on both
 * halves of ASDOUT, whereas 5 would inject a digital copy of the DAC output
 * into the ADC stream, which is a digital feedback path and not an analog
 * loopback. Bit 3, like bit 5 of 0x16, is not in the user guide's register
 * table; the vendor reference drivers set it and describe it as improving I2C
 * noise immunity.
 */
#define ES8311_GPIO_ADCDAT_ADC 0x08U

/*
 * The sample rates that keep the 256fs relationship. The user guide gives 8 kHz
 * to 48 kHz as the single-speed range; above that the ADC would need
 * double-speed mode, which this driver does not program.
 */
static const uint32_t es8311_rates[] = {
	8000U, 11025U, 12000U, 16000U, 22050U, 24000U, 32000U, 44100U, 48000U,
};

struct es8311_config {
	struct i2c_dt_spec bus;
	struct gpio_dt_spec reset_gpio;
	struct gpio_dt_spec enable_gpio;
};

struct es8311_data {
	struct k_mutex lock;
	uint8_t dac_volume_code; /* cached 0x32 */
	uint8_t adc_volume_code; /* cached 0x17 */
	bool dac_mute;
	bool adc_mute;
};

static int es8311_reg_write(const struct device *dev, uint8_t reg, uint8_t val)
{
	const struct es8311_config *cfg = dev->config;

	return i2c_reg_write_byte_dt(&cfg->bus, reg, val);
}

static int es8311_reg_read(const struct device *dev, uint8_t reg, uint8_t *val)
{
	const struct es8311_config *cfg = dev->config;

	return i2c_reg_read_byte_dt(&cfg->bus, reg, val);
}

static int es8311_reg_update(const struct device *dev, uint8_t reg, uint8_t mask, uint8_t val)
{
	const struct es8311_config *cfg = dev->config;

	return i2c_reg_update_byte_dt(&cfg->bus, reg, mask, val);
}

static bool es8311_rate_supported(uint32_t rate)
{
	for (size_t i = 0; i < ARRAY_SIZE(es8311_rates); i++) {
		if (es8311_rates[i] == rate) {
			return true;
		}
	}

	return false;
}

/* Convert a dB volume level to a digital volume register code. */
static uint8_t es8311_db_to_code(int db)
{
	int code;

	if (db > ES8311_VOL_DB_MAX) {
		db = ES8311_VOL_DB_MAX;
	} else if (db < ES8311_VOL_DB_MIN) {
		db = ES8311_VOL_DB_MIN;
	}

	code = (int)ES8311_VOL_0DB_CODE + (db * 2);
	if (code < 0) {
		code = 0;
	} else if (code > 0xFF) {
		code = 0xFF;
	}

	return (uint8_t)code;
}

static int es8311_configure(const struct device *dev, struct audio_codec_cfg *cfg)
{
	struct es8311_data *data = dev->data;
	uint32_t rate;
	uint8_t sdp_out;
	bool playback = false;
	bool capture = false;
	int ret = 0;

	if (cfg->dai_type != AUDIO_DAI_TYPE_I2S) {
		LOG_INF("Unsupported DAI type %d", cfg->dai_type);
		return -ENOTSUP;
	}

	switch (cfg->dai_route) {
	case AUDIO_ROUTE_PLAYBACK:
		playback = true;
		break;
	case AUDIO_ROUTE_CAPTURE:
		capture = true;
		break;
	case AUDIO_ROUTE_PLAYBACK_CAPTURE:
		playback = true;
		capture = true;
		break;
	default:
		LOG_INF("Unsupported route %u (playback/capture only)", cfg->dai_route);
		return -ENOTSUP;
	}

	/*
	 * Only the standard (Philips) I2S format with the default clock polarity
	 * and MSB-first ordering is supported: the serial data ports are
	 * programmed for exactly that.
	 */
	if ((cfg->dai_cfg.i2s.format & I2S_FMT_DATA_FORMAT_MASK) != I2S_FMT_DATA_FORMAT_I2S) {
		LOG_INF("Unsupported I2S data format 0x%x (only standard I2S)",
			cfg->dai_cfg.i2s.format & I2S_FMT_DATA_FORMAT_MASK);
		return -ENOTSUP;
	}

	if ((cfg->dai_cfg.i2s.format & I2S_FMT_DATA_ORDER_LSB) != 0U) {
		LOG_INF("LSB-first data ordering not supported");
		return -ENOTSUP;
	}

	if ((cfg->dai_cfg.i2s.format & I2S_FMT_CLK_FORMAT_MASK) != I2S_FMT_CLK_NF_NB) {
		LOG_INF("Unsupported I2S clock format 0x%x (only NF_NB)",
			cfg->dai_cfg.i2s.format & I2S_FMT_CLK_FORMAT_MASK);
		return -ENOTSUP;
	}

	/*
	 * The master clock is derived from BCLK, so the frame has to carry 32 bit
	 * clocks for the clock tree to land on 256fs. Only a 16-bit word does.
	 */
	if (cfg->dai_cfg.i2s.word_size != AUDIO_PCM_WIDTH_16_BITS) {
		LOG_INF("Unsupported word size %u (BCLK-derived MCLK needs a 16-bit word)",
			cfg->dai_cfg.i2s.word_size);
		return -ENOTSUP;
	}

	rate = cfg->dai_cfg.i2s.frame_clk_freq;
	if (!es8311_rate_supported(rate)) {
		LOG_INF("Unsupported sample rate %u", rate);
		return -ENOTSUP;
	}

	/*
	 * mclk_freq describes the master clock the codec ends up running at. It
	 * is derived from BCLK, so the only usable value is the 256fs one the
	 * dividers are programmed for; zero means "derive it".
	 */
	if (cfg->mclk_freq != 0U && cfg->mclk_freq != (rate * 256U)) {
		LOG_INF("Unsupported mclk %u for rate %u (expected %u or 0)", cfg->mclk_freq, rate,
			rate * 256U);
		return -ENOTSUP;
	}

	sdp_out = ES8311_SDP_I2S_16BIT | (data->adc_mute ? ES8311_SDP_MUTE : 0U);

	LOG_DBG("Configure: rate=%u mclk=%u", rate, cfg->mclk_freq);

	k_mutex_lock(&data->lock, K_FOREVER);

	/* Release reset and power up the clock state machine. */
	ret = es8311_reg_write(dev, ES8311_REG_RESET, ES8311_RESET_CSM_ON);
	if (ret < 0) {
		goto end;
	}
	k_msleep(ES8311_RESET_DELAY_MS);

	ret = es8311_reg_write(dev, ES8311_REG_CLK_MANAGER, ES8311_CLK_MGR_FROM_BCLK);
	if (ret < 0) {
		goto end;
	}

	ret = es8311_reg_write(dev, ES8311_REG_CLK_PRE, ES8311_CLK_PRE_DIV1_MULT8);
	if (ret < 0) {
		goto end;
	}
	ret = es8311_reg_write(dev, ES8311_REG_ADC_OSR, ES8311_ADC_OSR_SINGLE_16);
	if (ret < 0) {
		goto end;
	}
	ret = es8311_reg_write(dev, ES8311_REG_DAC_OSR, ES8311_DAC_OSR_16);
	if (ret < 0) {
		goto end;
	}
	ret = es8311_reg_write(dev, ES8311_REG_CLK_DIV, ES8311_CLK_DIV_ADC1_DAC1);
	if (ret < 0) {
		goto end;
	}
	ret = es8311_reg_write(dev, ES8311_REG_CLK_BCLK, ES8311_BCLK_SLAVE_DIV4);
	if (ret < 0) {
		goto end;
	}
	ret = es8311_reg_write(dev, ES8311_REG_CLK_LRCK_H, ES8311_LRCK_DIV_H);
	if (ret < 0) {
		goto end;
	}
	ret = es8311_reg_write(dev, ES8311_REG_CLK_LRCK_L, ES8311_LRCK_DIV_L);
	if (ret < 0) {
		goto end;
	}

	if (playback) {
		ret = es8311_reg_write(dev, ES8311_REG_SDP_IN, ES8311_SDP_I2S_16BIT);
		if (ret < 0) {
			goto end;
		}
	}

	/* Bias, reference and mid-rail are shared by the DAC and the ADC. */
	ret = es8311_reg_write(dev, ES8311_REG_SYSTEM_0D, ES8311_ANALOG_PWR_ON);
	if (ret < 0) {
		goto end;
	}
	k_msleep(ES8311_PWR_UP_DELAY_MS);

	if (playback) {
		ret = es8311_reg_write(dev, ES8311_REG_SYSTEM_12, ES8311_DAC_PWR_ON);
		if (ret < 0) {
			goto end;
		}

		ret = es8311_reg_write(dev, ES8311_REG_SYSTEM_13, ES8311_OUT_HEADPHONE);
		if (ret < 0) {
			goto end;
		}

		ret = es8311_reg_write(dev, ES8311_REG_DAC_VOLUME, data->dac_volume_code);
		if (ret < 0) {
			goto end;
		}

		ret = es8311_reg_write(dev, ES8311_REG_DAC_EQ, ES8311_DAC_EQ_BYPASS);
		if (ret < 0) {
			goto end;
		}

		ret = es8311_reg_write(dev, ES8311_REG_DAC_MUTE,
				       data->dac_mute ? ES8311_DAC_MUTE_ON : ES8311_DAC_MUTE_OFF);
		if (ret < 0) {
			goto end;
		}
	}

	if (capture) {
		/*
		 * Capture is always on once configured: the codec has no separate
		 * capture enable, and the application simply reads the I2S receive
		 * stream. This mirrors the in-tree wm8904 and da7212 codecs.
		 */
		ret = es8311_reg_write(dev, ES8311_REG_SDP_OUT, sdp_out);
		if (ret < 0) {
			goto end;
		}

		ret = es8311_reg_write(dev, ES8311_REG_SYSTEM_0E, ES8311_ADC_PWR_ON);
		if (ret < 0) {
			goto end;
		}

		ret = es8311_reg_write(dev, ES8311_REG_ADC_PGA, ES8311_ADC_PGA_MIC1_30DB);
		if (ret < 0) {
			goto end;
		}

		ret = es8311_reg_write(dev, ES8311_REG_ADC_RAMP, ES8311_ADC_RAMP_RATE);
		if (ret < 0) {
			goto end;
		}

		ret = es8311_reg_write(dev, ES8311_REG_ADC_SCALE, ES8311_ADC_SCALE_24DB);
		if (ret < 0) {
			goto end;
		}

		ret = es8311_reg_write(dev, ES8311_REG_ADC_VOLUME, data->adc_volume_code);
		if (ret < 0) {
			goto end;
		}

		/* The high-pass filter cancels the ADC's digital DC offset. */
		ret = es8311_reg_write(dev, ES8311_REG_ADC_HPF1, ES8311_ADC_HPF1_VAL);
		if (ret < 0) {
			goto end;
		}
		ret = es8311_reg_write(dev, ES8311_REG_ADC_HPF2, ES8311_ADC_HPF2_DCBLOCK);
		if (ret < 0) {
			goto end;
		}

		ret = es8311_reg_write(dev, ES8311_REG_GPIO, ES8311_GPIO_ADCDAT_ADC);
		if (ret < 0) {
			goto end;
		}

		ret = es8311_reg_write(dev, ES8311_REG_ADC_GP45, ES8311_ADC_GP45_DEFAULT);
	}

end:
	k_mutex_unlock(&data->lock);

	if (ret < 0) {
		LOG_ERR("configure() I2C error: %d", ret);
	}

	return ret;
}

static void es8311_start_output(const struct device *dev)
{
	struct es8311_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	data->dac_mute = false;
	ret = es8311_reg_update(dev, ES8311_REG_DAC_MUTE, ES8311_DAC_MUTE_MASK,
				ES8311_DAC_MUTE_OFF);
	k_mutex_unlock(&data->lock);

	if (ret < 0) {
		LOG_ERR("start_output: failed to unmute (%d)", ret);
	}
}

static void es8311_stop_output(const struct device *dev)
{
	struct es8311_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	data->dac_mute = true;
	ret = es8311_reg_update(dev, ES8311_REG_DAC_MUTE, ES8311_DAC_MUTE_MASK, ES8311_DAC_MUTE_ON);
	k_mutex_unlock(&data->lock);

	if (ret < 0) {
		LOG_ERR("stop_output: failed to mute (%d)", ret);
	}
}

static int es8311_set_property(const struct device *dev, audio_property_t property,
			       audio_channel_t channel, audio_property_value_t val)
{
	struct es8311_data *data = dev->data;
	int ret = 0;

	if (channel != AUDIO_CHANNEL_ALL && channel != AUDIO_CHANNEL_FRONT_LEFT &&
	    channel != AUDIO_CHANNEL_FRONT_RIGHT) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	switch (property) {
	case AUDIO_PROPERTY_OUTPUT_VOLUME:
		data->dac_volume_code = es8311_db_to_code(val.vol);
		break;
	case AUDIO_PROPERTY_OUTPUT_MUTE:
		data->dac_mute = val.mute;
		break;
	case AUDIO_PROPERTY_INPUT_VOLUME:
		data->adc_volume_code = es8311_db_to_code(val.vol);
		break;
	case AUDIO_PROPERTY_INPUT_MUTE:
		data->adc_mute = val.mute;
		break;
	default:
		ret = -ENOTSUP;
		break;
	}

	k_mutex_unlock(&data->lock);

	return ret;
}

static int es8311_apply_properties(const struct device *dev)
{
	struct es8311_data *data = dev->data;
	uint8_t dac_volume_code;
	uint8_t adc_volume_code;
	bool dac_mute;
	bool adc_mute;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	dac_volume_code = data->dac_volume_code;
	adc_volume_code = data->adc_volume_code;
	dac_mute = data->dac_mute;
	adc_mute = data->adc_mute;
	k_mutex_unlock(&data->lock);

	ret = es8311_reg_write(dev, ES8311_REG_DAC_VOLUME, dac_volume_code);
	if (ret < 0) {
		LOG_ERR("Failed to set DAC volume 0x%02x (%d)", dac_volume_code, ret);
		return ret;
	}

	ret = es8311_reg_update(dev, ES8311_REG_DAC_MUTE, ES8311_DAC_MUTE_MASK,
				dac_mute ? ES8311_DAC_MUTE_ON : ES8311_DAC_MUTE_OFF);
	if (ret < 0) {
		LOG_ERR("Failed to set DAC mute %d (%d)", dac_mute, ret);
		return ret;
	}

	ret = es8311_reg_write(dev, ES8311_REG_ADC_VOLUME, adc_volume_code);
	if (ret < 0) {
		LOG_ERR("Failed to set ADC volume 0x%02x (%d)", adc_volume_code, ret);
		return ret;
	}

	/* The ADC is muted at its serial data port rather than by its volume. */
	ret = es8311_reg_update(dev, ES8311_REG_SDP_OUT, ES8311_SDP_MUTE,
				adc_mute ? ES8311_SDP_MUTE : 0U);
	if (ret < 0) {
		LOG_ERR("Failed to set ADC mute %d (%d)", adc_mute, ret);
		return ret;
	}

	return 0;
}

/*
 * route_input() and route_output() are deliberately not implemented: the ES8311
 * has one differential microphone input and one output, so there is nothing to
 * multiplex.
 */
static const struct audio_codec_api es8311_api = {
	.configure = es8311_configure,
	.start_output = es8311_start_output,
	.stop_output = es8311_stop_output,
	.set_property = es8311_set_property,
	.apply_properties = es8311_apply_properties,
};

static int es8311_check_id(const struct device *dev)
{
	uint8_t id1 = 0U;
	uint8_t id2 = 0U;
	int ret;

	ret = es8311_reg_read(dev, ES8311_REG_CHIP_ID1, &id1);
	if (ret < 0) {
		LOG_ERR("Failed to read chip id1 (%d)", ret);
		return ret;
	}

	ret = es8311_reg_read(dev, ES8311_REG_CHIP_ID2, &id2);
	if (ret < 0) {
		LOG_ERR("Failed to read chip id2 (%d)", ret);
		return ret;
	}

	if (id1 != ES8311_CHIP_ID1 || id2 != ES8311_CHIP_ID2) {
		LOG_WRN("Unexpected chip id 0x%02x%02x (expected 0x%02x%02x)", id1, id2,
			ES8311_CHIP_ID1, ES8311_CHIP_ID2);
	}

	return 0;
}

static int es8311_init(const struct device *dev)
{
	const struct es8311_config *cfg = dev->config;
	struct es8311_data *data = dev->data;
	int ret;

	if (!i2c_is_ready_dt(&cfg->bus)) {
		LOG_ERR("I2C controller not ready");
		return -ENODEV;
	}

	k_mutex_init(&data->lock);
	data->dac_volume_code = ES8311_VOL_DEFAULT_CODE;
	data->adc_volume_code = ES8311_VOL_0DB_CODE;
	data->dac_mute = false;
	data->adc_mute = false;

	if (cfg->enable_gpio.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->enable_gpio)) {
			LOG_ERR("Enable GPIO not ready");
			return -ENODEV;
		}

		ret = gpio_pin_configure_dt(&cfg->enable_gpio, GPIO_OUTPUT_ACTIVE);
		if (ret < 0) {
			LOG_ERR("Failed to configure enable GPIO (%d)", ret);
			return ret;
		}
	}

	if (cfg->reset_gpio.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->reset_gpio)) {
			LOG_ERR("Reset GPIO not ready");
			return -ENODEV;
		}

		/* Assert reset, settle, then release it. */
		ret = gpio_pin_configure_dt(&cfg->reset_gpio, GPIO_OUTPUT_ACTIVE);
		if (ret < 0) {
			LOG_ERR("Failed to configure reset GPIO (%d)", ret);
			return ret;
		}

		k_msleep(ES8311_GPIO_DELAY_MS);

		ret = gpio_pin_set_dt(&cfg->reset_gpio, 0);
		if (ret < 0) {
			LOG_ERR("Failed to deassert reset GPIO (%d)", ret);
			return ret;
		}

		k_msleep(ES8311_RESET_DELAY_MS);
	}

	return es8311_check_id(dev);
}

#define ES8311_INST(idx)                                                                           \
	static const struct es8311_config es8311_config_##idx = {                                  \
		.bus = I2C_DT_SPEC_INST_GET(idx),                                                  \
		.reset_gpio = GPIO_DT_SPEC_INST_GET_OR(idx, reset_gpios, {0}),                     \
		.enable_gpio = GPIO_DT_SPEC_INST_GET_OR(idx, enable_gpios, {0}),                   \
	};                                                                                         \
	static struct es8311_data es8311_data_##idx;                                               \
	DEVICE_DT_INST_DEFINE(idx, es8311_init, NULL, &es8311_data_##idx, &es8311_config_##idx,    \
			      POST_KERNEL, CONFIG_AUDIO_CODEC_INIT_PRIORITY, &es8311_api)

DT_INST_FOREACH_STATUS_OKAY(ES8311_INST)
