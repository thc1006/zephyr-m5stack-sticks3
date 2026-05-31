/*
 * Copyright (c) 2026 Hsiu-Chi Tsai
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Everest Semiconductor ES8311 mono audio codec driver.
 *
 * Control interface: I2C (7-bit address 0x18 by default). Audio interface:
 * I2S / PCM. The codec can run its internal clock tree from a dedicated MCLK
 * pin or from the I2S bit clock (BCLK); the M5Stack StickS3 bring-up uses the
 * MCLK-from-BCLK mode.
 *
 * Register sequence source: the 16 kHz / 16-bit playback sequence applied by
 * configure() is the sequence runtime-verified on the StickS3 hardware in this
 * project's bring-up, cross-checked against the M5Stack M5GFX/M5Unified vendor
 * codec init and the Everest ES8311 user guide (clock coefficient table). Only
 * the 16 kHz case is hardware-validated; other rates in es8311_coeff_table are
 * derived from the ES8311 user guide and are NOT yet hardware-confirmed.
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

/* Register map (subset used by this driver). */
#define ES8311_REG_RESET      0x00U /* reset / CSM power on */
#define ES8311_REG_CLK_MANAGER 0x01U /* clock manager: MCLK source, clock enables */
#define ES8311_REG_CLK_DIV1   0x02U /* pre_div / pre_multi */
#define ES8311_REG_CLK_DIV2   0x03U /* adc_div / fs_mode */
#define ES8311_REG_CLK_DIV3   0x04U /* dac_div */
#define ES8311_REG_CLK_DIV4   0x05U /* adc/dac osr */
#define ES8311_REG_CLK_BCLK   0x06U /* bclk divider (slave: bclk_div) */
#define ES8311_REG_CLK_LRCK_H 0x07U /* LRCK divider high */
#define ES8311_REG_CLK_LRCK_L 0x08U /* LRCK divider low */
#define ES8311_REG_SDP_IN     0x09U /* serial data port (DAC path) format */
#define ES8311_REG_SDP_OUT    0x0AU /* serial data port (ADC path) format */
#define ES8311_REG_SYSTEM_0D  0x0DU /* power up/down: analog/charge pump */
#define ES8311_REG_SYSTEM_12  0x12U /* power up/down: DAC */
#define ES8311_REG_SYSTEM_13  0x13U /* output / headphone drive */
#define ES8311_REG_DAC_MUTE   0x31U /* DAC mute control */
#define ES8311_REG_DAC_VOLUME 0x32U /* DAC digital volume */
#define ES8311_REG_DAC_EQ     0x37U /* DAC EQ bypass */
#define ES8311_REG_CHIP_ID1   0xFDU /* chip id high: 0x83 */
#define ES8311_REG_CHIP_ID2   0xFEU /* chip id low: 0x11 */

#define ES8311_CHIP_ID1       0x83U
#define ES8311_CHIP_ID2       0x11U

/* Bit fields. */
#define ES8311_RESET_CSM_ON      0x80U /* CSM power up, reset released */
/*
 * DAC mute: bit6 DAC_DSMMUTE + bit5 DAC_DEMMUTE (ES8311 datasheet reg 0x31,
 * matches Espressif esp-bsp / esp_codec_dev). The earlier 0x07 was wrong - it
 * set bits[2:0], which are not the mute field, so stop_output() never actually
 * muted the DAC (masked on the StickS3 beep only because the AW8737 amp gates
 * the speaker). Caught by cross-validation against the datasheet, not the ztest.
 */
#define ES8311_DAC_MUTE_MASK     0x60U
#define ES8311_DAC_MUTE_ON       0x60U
#define ES8311_DAC_MUTE_OFF      0x00U

/* Settle delays (ms). */
#define ES8311_RESET_DELAY_MS    10
#define ES8311_PWR_UP_DELAY_MS   10
#define ES8311_GPIO_DELAY_MS     1

/*
 * DAC digital volume range. Register 0x32 is a linear code: 0x00 == mute /
 * minimum, 0xFF == maximum (+32 dB), with 0.5 dB per step above 0. The codec
 * API set_property() volume is expressed in dB; we clamp to the usable range
 * and map to the register code. 0 dB corresponds to code 0xBF.
 */
#define ES8311_VOL_DB_MAX        32
#define ES8311_VOL_DB_MIN        (-95)
#define ES8311_VOL_0DB_CODE      0xBFU
#define ES8311_VOL_DEFAULT_CODE  0xC0U /* ~+0.5 dB; project bring-up default */

struct es8311_config {
	struct i2c_dt_spec bus;
	struct gpio_dt_spec reset_gpio;
	struct gpio_dt_spec enable_gpio;
};

struct es8311_data {
	struct k_mutex lock;
	uint8_t volume_code; /* cached DAC volume register value */
	bool mute;
};

/*
 * Clock coefficient entry. For a given (mclk, sample_rate) the codec needs a
 * specific divider/multiplier programming. Only the StickS3-proven 16 kHz
 * MCLK-from-BCLK case (MCLK = BCLK*8 = 4.096 MHz, LRCK = MCLK/256) is
 * hardware-validated; the remaining rows follow the ES8311 user guide.
 */
struct es8311_coeff {
	uint32_t mclk;        /* master clock fed to the codec, Hz */
	uint32_t rate;        /* PCM sample rate, Hz */
	uint8_t pre_div;      /* 0x02: pre divider / multiplier field */
	uint8_t adc_div;      /* 0x03 */
	uint8_t dac_div;      /* 0x04 */
	uint8_t osr;          /* 0x05 */
	uint8_t bclk_div;     /* 0x06 */
	uint8_t lrck_h;       /* 0x07 */
	uint8_t lrck_l;       /* 0x08 */
};

/*
 * MCLK = 256 * Fs is the canonical "256fs" relationship used by these rows.
 * For the StickS3 case MCLK is derived from BCLK (MCLK = BCLK*8): with 16-bit
 * stereo, BCLK = 32*Fs, so MCLK = 8*32*Fs = 256*Fs = 4.096 MHz at 16 kHz; the
 * pre_multi x8 + LRCK/256 programming below reproduces the verified register
 * dump.
 */
static const struct es8311_coeff es8311_coeff_table[] = {
	/* StickS3-proven: 16 kHz, MCLK = BCLK*8 = 4.096 MHz, LRCK = MCLK/256. */
	{
		.mclk = 4096000U,
		.rate = 16000U,
		.pre_div = 0x18U, /* pre_div=1, pre_multi=x8 */
		.adc_div = 0x10U,
		.dac_div = 0x10U,
		.osr = 0x00U,
		.bclk_div = 0x03U,
		.lrck_h = 0x00U,
		/*
		 * lrck_l low byte; 0xFF is the value taken from the verified
		 * StickS3 register dump that encodes the MCLK/256 (256fs) ratio.
		 */
		.lrck_l = 0xFFU,
	},
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

/*
 * Look up the clock-coefficient row for (mclk, rate). A caller-supplied
 * mclk == 0 means "MCLK is derived from BCLK; match the first row for that
 * rate" -- this is safe only while the table holds at most one row per rate.
 * If multiple MCLK options per rate are ever added, mclk == 0 would become
 * ambiguous and the caller must pass an explicit mclk.
 */
static const struct es8311_coeff *es8311_get_coeff(uint32_t mclk, uint32_t rate)
{
	for (size_t i = 0; i < ARRAY_SIZE(es8311_coeff_table); i++) {
		if (es8311_coeff_table[i].rate == rate &&
		    (mclk == 0U || es8311_coeff_table[i].mclk == mclk)) {
			return &es8311_coeff_table[i];
		}
	}

	return NULL;
}

/* Map a 16-bit I2S data format to the SDP word-length field (bits [4:2]). */
static int es8311_sdp_wordlen(audio_pcm_width_t width, uint8_t *field)
{
	switch (width) {
	case AUDIO_PCM_WIDTH_16_BITS:
		*field = 0x03U; /* 16-bit */
		break;
	case AUDIO_PCM_WIDTH_24_BITS:
		*field = 0x00U; /* 24-bit */
		break;
	case AUDIO_PCM_WIDTH_32_BITS:
		*field = 0x04U; /* 32-bit */
		break;
	default:
		LOG_INF("Unsupported word size %u", width);
		return -ENOTSUP;
	}

	return 0;
}

static int es8311_configure(const struct device *dev, struct audio_codec_cfg *cfg)
{
	struct es8311_data *data = dev->data;
	const struct es8311_coeff *coeff;
	uint32_t rate;
	audio_pcm_width_t width;
	uint8_t wordlen;
	uint8_t sdp;
	int ret;

	if (cfg->dai_type != AUDIO_DAI_TYPE_I2S) {
		LOG_INF("Unsupported DAI type %d", cfg->dai_type);
		return -ENOTSUP;
	}

	if (cfg->dai_route != AUDIO_ROUTE_PLAYBACK) {
		LOG_INF("Unsupported route %u (only playback)", cfg->dai_route);
		return -ENOTSUP;
	}

	/*
	 * Only the standard (Philips) I2S data format with the default clock
	 * polarity and MSB-first ordering is supported; the proven register
	 * sequence below programs the serial-data port for that format. Reject
	 * left/right-justified, LSB-first, or inverted bit/frame-clock requests
	 * (mirrors the format/option checks in aw88298.c).
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

	rate = cfg->dai_cfg.i2s.frame_clk_freq;
	width = cfg->dai_cfg.i2s.word_size;
	/*
	 * cfg->dai_cfg.i2s.channels is intentionally unused: the ES8311 is a
	 * mono codec, so the stereo/mono frame layout on the I2S bus does not
	 * change the codec's register programming here.
	 */

	coeff = es8311_get_coeff(cfg->mclk_freq, rate);
	if (coeff == NULL) {
		LOG_INF("No clock coefficients for mclk=%u rate=%u", cfg->mclk_freq, rate);
		return -ENOTSUP;
	}

	ret = es8311_sdp_wordlen(width, &wordlen);
	if (ret < 0) {
		return ret;
	}

	/* I2S format, word length in [4:2]. 0x0C = standard I2S + 16-bit. */
	sdp = (wordlen << 2);

	LOG_DBG("Configure: rate=%u width=%u mclk=%u", rate, width, cfg->mclk_freq);

	k_mutex_lock(&data->lock, K_FOREVER);

	/*
	 * Hardware-verified StickS3 16 kHz / 16-bit / MCLK-from-BCLK sequence.
	 * See the file header for the source of these register values.
	 */
	/* Release reset / power up the state machine. */
	ret = es8311_reg_write(dev, ES8311_REG_RESET, ES8311_RESET_CSM_ON);
	if (ret < 0) {
		goto end;
	}
	k_msleep(ES8311_RESET_DELAY_MS);

	/* Clock manager: MCLK from BCLK (bit7=1), all internal clocks on. */
	ret = es8311_reg_write(dev, ES8311_REG_CLK_MANAGER, 0xBFU);
	if (ret < 0) {
		goto end;
	}

	/* Clock dividers from the coefficient table. */
	ret = es8311_reg_write(dev, ES8311_REG_CLK_DIV1, coeff->pre_div);
	if (ret < 0) {
		goto end;
	}
	ret = es8311_reg_write(dev, ES8311_REG_CLK_DIV2, coeff->adc_div);
	if (ret < 0) {
		goto end;
	}
	ret = es8311_reg_write(dev, ES8311_REG_CLK_DIV3, coeff->dac_div);
	if (ret < 0) {
		goto end;
	}
	ret = es8311_reg_write(dev, ES8311_REG_CLK_DIV4, coeff->osr);
	if (ret < 0) {
		goto end;
	}
	ret = es8311_reg_write(dev, ES8311_REG_CLK_BCLK, coeff->bclk_div);
	if (ret < 0) {
		goto end;
	}
	ret = es8311_reg_write(dev, ES8311_REG_CLK_LRCK_H, coeff->lrck_h);
	if (ret < 0) {
		goto end;
	}
	ret = es8311_reg_write(dev, ES8311_REG_CLK_LRCK_L, coeff->lrck_l);
	if (ret < 0) {
		goto end;
	}

	/* Serial data port: I2S format + word length. */
	ret = es8311_reg_write(dev, ES8311_REG_SDP_IN, sdp);
	if (ret < 0) {
		goto end;
	}

	/* Power up analog + charge pump, then settle. */
	ret = es8311_reg_write(dev, ES8311_REG_SYSTEM_0D, 0x01U);
	if (ret < 0) {
		goto end;
	}
	k_msleep(ES8311_PWR_UP_DELAY_MS);

	/* Power up DAC. */
	ret = es8311_reg_write(dev, ES8311_REG_SYSTEM_12, 0x00U);
	if (ret < 0) {
		goto end;
	}

	/* DAC output / headphone drive. */
	ret = es8311_reg_write(dev, ES8311_REG_SYSTEM_13, 0x10U);
	if (ret < 0) {
		goto end;
	}

	/* DAC digital volume (cached default or set_property override). */
	ret = es8311_reg_write(dev, ES8311_REG_DAC_VOLUME, data->volume_code);
	if (ret < 0) {
		goto end;
	}

	/* Bypass DAC EQ. */
	ret = es8311_reg_write(dev, ES8311_REG_DAC_EQ, 0x08U);
	if (ret < 0) {
		goto end;
	}

	/* Unmute (or re-apply cached mute state). */
	ret = es8311_reg_write(dev, ES8311_REG_DAC_MUTE,
			       data->mute ? ES8311_DAC_MUTE_ON : ES8311_DAC_MUTE_OFF);

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
	data->mute = false;
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
	data->mute = true;
	ret = es8311_reg_update(dev, ES8311_REG_DAC_MUTE, ES8311_DAC_MUTE_MASK,
				ES8311_DAC_MUTE_ON);
	k_mutex_unlock(&data->lock);

	if (ret < 0) {
		LOG_ERR("stop_output: failed to mute (%d)", ret);
	}
}

/* Convert a dB volume level to the ES8311 0x32 register code. */
static uint8_t es8311_db_to_code(int db)
{
	int code;

	if (db > ES8311_VOL_DB_MAX) {
		db = ES8311_VOL_DB_MAX;
	} else if (db < ES8311_VOL_DB_MIN) {
		db = ES8311_VOL_DB_MIN;
	}

	/* 0 dB == 0xBF, 0.5 dB per step. */
	code = (int)ES8311_VOL_0DB_CODE + (db * 2);
	if (code < 0) {
		code = 0;
	} else if (code > 0xFF) {
		code = 0xFF;
	}

	return (uint8_t)code;
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
		data->volume_code = es8311_db_to_code(val.vol);
		break;
	case AUDIO_PROPERTY_OUTPUT_MUTE:
		data->mute = val.mute;
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
	uint8_t volume_code;
	bool mute;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	volume_code = data->volume_code;
	mute = data->mute;
	k_mutex_unlock(&data->lock);

	ret = es8311_reg_write(dev, ES8311_REG_DAC_VOLUME, volume_code);
	if (ret < 0) {
		LOG_ERR("Failed to set DAC volume 0x%02x (%d)", volume_code, ret);
		return ret;
	}

	ret = es8311_reg_update(dev, ES8311_REG_DAC_MUTE, ES8311_DAC_MUTE_MASK,
				mute ? ES8311_DAC_MUTE_ON : ES8311_DAC_MUTE_OFF);
	if (ret < 0) {
		LOG_ERR("Failed to set mute %d (%d)", mute, ret);
		return ret;
	}

	return 0;
}

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
	data->volume_code = ES8311_VOL_DEFAULT_CODE;
	data->mute = false;

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

		/* Assert reset (active low), settle, then release. */
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
