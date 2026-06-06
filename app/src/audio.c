/*
 * Copyright (c) 2026 Hsiu-Chi Tsai
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * P5: ES8311 audio bring-up for the M5StickS3 validation app.
 *
 * Everything here is gated behind CONFIG_APP_AUDIO (built via
 * overlay-audio.conf); the default build does not compile this file.
 *
 * Signal path: SoC I2S0 (master, 16 kHz / 16-bit / standard I2S) -> ES8311
 * codec (I2C control @ 0x18, MCLK derived from BCLK) -> AW8737 speaker amp.
 * The amp is enabled by the M5PM1 PMIC GPIO3 (sound_amp / amp-gpios) and is
 * driven high ONLY for the duration of a beep to avoid switch-on/off pops and
 * to keep the speaker path muted at rest.
 *
 * BUILD-VERIFIED ONLY (compile + link); not flashed. The trigger ordering
 * below follows the anti-click sequence proven during the throwaway bring-up,
 * but the amp/power path must be reviewed before any flash.
 */

#include "audio.h"

#ifdef CONFIG_APP_AUDIO

#include "audio_dsp.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/audio/codec.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/logging/log.h>

#include <string.h>

LOG_MODULE_REGISTER(audio, LOG_LEVEL_INF);

/* Audio format: 16 kHz / 16-bit / stereo (the esp32-i2s driver needs 2 ch). */
#define AUDIO_SAMPLE_RATE 16000U
#define AUDIO_WORD_BITS   16U
#define AUDIO_CHANNELS    2U
#define AUDIO_FRAME_BYTES (AUDIO_CHANNELS * (AUDIO_WORD_BITS / 8U)) /* 4 */

/* Conservative output: ~ -15 dBFS of 16-bit full scale (32767). */
#define TONE_AMPLITUDE 5800

/* 440 Hz beep, ~200 ms. */
#define TONE_FREQ_HZ   440U
#define TONE_MS        200U

/* Safe low playback volume in dB (ES8311 set_property maps dB -> reg code). */
#define AUDIO_VOLUME_DB (-20)

/*
 * I2S TX memory blocks. block_size is a multiple of 4 (one stereo 16-bit frame
 * = 4 bytes). 256 frames per block = 1024 bytes ~= 16 ms at 16 kHz. The slab
 * lives in .bss -> internal SRAM (zephyr,sram = &sram1), never PSRAM, which
 * the I2S DMA requires. At least 2 blocks per the I2S API; we keep a small
 * pool so a couple of blocks can be pre-queued before START (anti-underrun).
 */
#define BLOCK_FRAMES 256U
#define BLOCK_SIZE   (BLOCK_FRAMES * AUDIO_FRAME_BYTES) /* 1024 bytes */
#define BLOCK_COUNT  4U

/* DMA-capable memory must be cache-line aligned on the esp32-i2s path. */
K_MEM_SLAB_DEFINE_STATIC(tx_slab, BLOCK_SIZE, BLOCK_COUNT, 32);

/* I2S RX (microphone capture) blocks: same geometry as TX. */
K_MEM_SLAB_DEFINE_STATIC(rx_slab, BLOCK_SIZE, BLOCK_COUNT, 32);

/* I2S write timeout (ms): long enough to block until a TX block frees up. */
#define I2S_WRITE_TIMEOUT_MS 1000

/*
 * Loopback I/O timeout (ms): bounds every RX read and TX slab alloc during the
 * self-test so a wedged shared clock fails fast (we then abort and cut the amp)
 * instead of hanging the caller. A block is ~16 ms, so 200 ms tolerates jitter
 * yet caps the worst case at one block-time per failed I/O. The TX-only
 * playback path (audio_beep) keeps the longer I2S_WRITE_TIMEOUT_MS.
 */
#define LOOP_IO_TIMEOUT_MS 200

/*
 * Mic capture (issue #6). The ES8311 ADC is mono and lands on one I2S slot, so
 * AUDIO_MIC_SLOT (a slot INDEX 0 or 1, not a channel count) picks it; HW-016d
 * confirmed slot 0 carries the ADC. AUDIO_MIC_FULL is the empirical full-scale
 * RMS for the PAGE_AUDIO live bar: HW-016d measured quiet ~70-100, a normal
 * voice a few hundred to a few thousand, and a loud clap >= 12000, so 2000 gives
 * visible bars for speech without pinning the bar to 4 on every breath.
 */
#define AUDIO_MIC_SLOT   0U
#define AUDIO_MIC_FULL   2000U
#define LOOP_SIL_BLOCKS  4U
#define LOOP_BEEP_BLOCKS 12U

/* Settle time after START before enabling the amp (clocks must be running). */
#define AMP_SETTLE_MS 20

static const struct device *const codec_dev = DEVICE_DT_GET(DT_NODELABEL(es8311));
static const struct device *const i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s0));
static const struct gpio_dt_spec amp_gpio =
	GPIO_DT_SPEC_GET(DT_NODELABEL(sound_amp), amp_gpios);

/* Written by audio_init() on the main thread, read by the capture thread too, so
 * volatile (a plain bool could be cached in the capture loop and never seen set).
 */
static volatile bool ready;

/*
 * Precomputed one-block 440 Hz mono sine, 16-bit signed, sampled at 16 kHz
 * with a running phase (so a continuous beep can loop the block back-to-back).
 * Amplitude is TONE_AMPLITUDE (~ -15 dBFS). A precomputed const table avoids
 * any floating-point / libm dependency at runtime. Generated offline; see the
 * P5 report. 16000/440 is not an integer so the block is not exactly periodic,
 * but for a short beep the tiny seam is inaudible.
 */
BUILD_ASSERT(TONE_FREQ_HZ == 440U && AUDIO_SAMPLE_RATE == 16000U &&
		     TONE_AMPLITUDE == 5800,
	     "tone table was generated for 440 Hz @ 16 kHz, amp 5800");

static const int16_t tone_mono[BLOCK_FRAMES] = {
	0, 997, 1965, 2874, 3697, 4410, 4992, 5426,
	5697, 5799, 5729, 5487, 5083, 4526, 3836, 3030,
	2135, 1176, 182, -817, -1792, -2714, -3555, -4290,
	-4897, -5359, -5660, -5794, -5754, -5544, -5168, -4638,
	-3970, -3184, -2303, -1354, -364, 636, 1618, 2552,
	3409, 4165, 4797, 5286, 5618, 5782, 5774, 5594,
	5248, 4745, 4101, 3335, 2470, 1530, 546, -455,
	-1442, -2387, -3260, -4036, -4692, -5209, -5570, -5765,
	-5789, -5640, -5323, -4848, -4228, -3482, -2633, -1705,
	-727, 273, 1265, 2220, 3108, 3903, 4583, 5126,
	5516, 5742, 5797, 5679, 5393, 4945, 4351, 3626,
	2794, 1879, 907, -91, -1087, -2050, -2952, -3767,
	-4469, -5038, -5457, -5714, -5800, -5714, -5457, -5038,
	-4469, -3767, -2952, -2050, -1087, -91, 907, 1879,
	2794, 3626, 4351, 4945, 5393, 5679, 5797, 5742,
	5516, 5126, 4583, 3903, 3108, 2220, 1265, 273,
	-727, -1705, -2633, -3482, -4228, -4848, -5323, -5640,
	-5789, -5765, -5570, -5209, -4692, -4036, -3260, -2387,
	-1442, -455, 546, 1530, 2470, 3335, 4101, 4745,
	5248, 5594, 5774, 5782, 5618, 5286, 4797, 4165,
	3409, 2552, 1618, 636, -364, -1354, -2303, -3184,
	-3970, -4638, -5168, -5544, -5754, -5794, -5660, -5359,
	-4897, -4290, -3555, -2714, -1792, -817, 182, 1176,
	2135, 3030, 3836, 4526, 5083, 5487, 5729, 5799,
	5697, 5426, 4992, 4410, 3697, 2874, 1965, 997,
	0, -997, -1965, -2874, -3697, -4410, -4992, -5426,
	-5697, -5799, -5729, -5487, -5083, -4526, -3836, -3030,
	-2135, -1176, -182, 817, 1792, 2714, 3555, 4290,
	4897, 5359, 5660, 5794, 5754, 5544, 5168, 4638,
	3970, 3184, 2303, 1354, 364, -636, -1618, -2552,
	-3409, -4165, -4797, -5286, -5618, -5782, -5774, -5594,
	-5248, -4745, -4101, -3335, -2470, -1530, -546, 455,
};

/* Stereo (L=R) expansion of tone_mono, fed to I2S. Filled once on first use. */
static int16_t tone_block[BLOCK_FRAMES * AUDIO_CHANNELS];
static bool tone_ready;

static void tone_block_fill(void)
{
	for (uint32_t i = 0; i < BLOCK_FRAMES; i++) {
		tone_block[i * AUDIO_CHANNELS] = tone_mono[i];      /* left  */
		tone_block[i * AUDIO_CHANNELS + 1U] = tone_mono[i]; /* right */
	}

	tone_ready = true;
}

int audio_init(void)
{
	struct audio_codec_cfg codec_cfg;
	struct i2s_config i2s_cfg;
	int ret;

	if (!device_is_ready(codec_dev)) {
		LOG_ERR("ES8311 codec not ready");
		return -ENODEV;
	}

	if (!device_is_ready(i2s_dev)) {
		LOG_ERR("I2S device not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&amp_gpio)) {
		LOG_ERR("Amp GPIO not ready");
		return -ENODEV;
	}

	/* Amp OFF until a beep: configure output-inactive (low). */
	ret = gpio_pin_configure_dt(&amp_gpio, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("Amp GPIO configure failed (%d)", ret);
		return ret;
	}

	/*
	 * Configure I2S0 TX as master (no I2S_OPT_*_CLK_TARGET => SoC drives
	 * BCLK + WS), standard I2S, 16 kHz / 16-bit / stereo. The esp32-i2s
	 * driver derives MCLK = 256 * Fs = 4.096 MHz, which matches the codec
	 * coefficient row (MCLK-from-BCLK, LRCK = MCLK/256).
	 */
	i2s_cfg.word_size = AUDIO_WORD_BITS;
	i2s_cfg.channels = AUDIO_CHANNELS;
	i2s_cfg.format = I2S_FMT_DATA_FORMAT_I2S;
	/* SoC is the I2S controller (master): drives BCLK + WS. */
	i2s_cfg.options = I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER;
	i2s_cfg.frame_clk_freq = AUDIO_SAMPLE_RATE;
	i2s_cfg.mem_slab = &tx_slab;
	i2s_cfg.block_size = BLOCK_SIZE;
	i2s_cfg.timeout = I2S_WRITE_TIMEOUT_MS;

	ret = i2s_configure(i2s_dev, I2S_DIR_TX, &i2s_cfg);
	if (ret < 0) {
		LOG_ERR("i2s_configure(TX) failed (%d)", ret);
		return ret;
	}

	/* Configure the ES8311 for the same 16 kHz / 16-bit I2S playback. */
	codec_cfg.mclk_freq = AUDIO_SAMPLE_RATE * 256U; /* 4.096 MHz */
	codec_cfg.dai_type = AUDIO_DAI_TYPE_I2S;
	codec_cfg.dai_route = AUDIO_ROUTE_PLAYBACK_CAPTURE;
	codec_cfg.dai_cfg.i2s = i2s_cfg;

	ret = audio_codec_configure(codec_dev, &codec_cfg);
	if (ret < 0) {
		LOG_ERR("audio_codec_configure failed (%d)", ret);
		return ret;
	}

	/*
	 * Configure I2S0 RX with the same format for microphone capture. The
	 * esp32-i2s driver shares BCLK/WS once both TX and RX are configured, so
	 * audio_loopback() can run full-duplex (I2S_DIR_BOTH). Only the mem_slab
	 * differs from the TX config above.
	 */
	i2s_cfg.mem_slab = &rx_slab;
	i2s_cfg.timeout = LOOP_IO_TIMEOUT_MS; /* bound a stalled mic read */
	ret = i2s_configure(i2s_dev, I2S_DIR_RX, &i2s_cfg);
	if (ret < 0) {
		LOG_ERR("i2s_configure(RX) failed (%d)", ret);
		return ret;
	}

	/* Safe low volume; leave the codec configured but the amp OFF. */
	audio_property_value_t vol = { .vol = AUDIO_VOLUME_DB };

	ret = audio_codec_set_property(codec_dev, AUDIO_PROPERTY_OUTPUT_VOLUME,
				       AUDIO_CHANNEL_ALL, vol);
	if (ret < 0) {
		LOG_WRN("set volume failed (%d); continuing", ret);
	}
	(void)audio_codec_apply_properties(codec_dev);

	if (!tone_ready) {
		tone_block_fill();
	}

	ready = true;
	LOG_INF("audio_init OK (16 kHz/16-bit, amp off)");
	return 0;
}

bool audio_ready(void)
{
	return ready;
}

void audio_beep(void)
{
	uint32_t total_frames;
	uint32_t blocks;
	int ret;

	if (!ready) {
		return;
	}

	if (!tone_ready) {
		tone_block_fill();
	}

	total_frames = (AUDIO_SAMPLE_RATE * TONE_MS) / 1000U;
	blocks = (total_frames + BLOCK_FRAMES - 1U) / BLOCK_FRAMES;
	if (blocks < 2U) {
		blocks = 2U; /* keep the TX queue primed */
	}

	/* Unmute the codec DAC for the duration of the beep. */
	audio_codec_start_output(codec_dev);

	/*
	 * Anti-click sequence:
	 *  1. Pre-queue two blocks while still stopped so the DMA never starves
	 *     at START.
	 *  2. START the I2S TX (clocks begin, DAC sees valid frames).
	 *  3. Let the clocks/codec settle, THEN enable the amp -> the speaker is
	 *     only ever driven once a clean signal is already flowing (no pop).
	 *  4. Feed the rest of the tone.
	 *  5. DRAIN (flush the queue + stop at block boundary).
	 *  6. Disable the amp BEFORE the codec mute so the amp is silenced first.
	 *  7. Mute the codec DAC.
	 */
	for (uint32_t i = 0; i < 2U; i++) {
		ret = i2s_buf_write(i2s_dev, tone_block, BLOCK_SIZE);
		if (ret < 0) {
			LOG_ERR("prequeue i2s_buf_write failed (%d)", ret);
			goto stop_amp;
		}
	}

	ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret < 0) {
		LOG_ERR("i2s_trigger(START) failed (%d)", ret);
		goto stop_amp;
	}

	k_msleep(AMP_SETTLE_MS);

	/* Speaker amp ON only now that valid frames are already streaming. */
	(void)gpio_pin_set_dt(&amp_gpio, 1);

	/* Feed the remaining blocks (we already queued 2). */
	for (uint32_t i = 2U; i < blocks; i++) {
		ret = i2s_buf_write(i2s_dev, tone_block, BLOCK_SIZE);
		if (ret < 0) {
			LOG_ERR("i2s_buf_write failed (%d)", ret);
			break;
		}
	}

	/* Flush whatever is queued and stop at the next block boundary. */
	ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
	if (ret < 0) {
		LOG_WRN("i2s_trigger(DRAIN) failed (%d)", ret);
	}

	/* Give the queued blocks time to drain before cutting the amp. */
	k_msleep(TONE_MS + AMP_SETTLE_MS);

stop_amp:
	/*
	 * Amp OFF first (silence the speaker), then mute the codec. Reached on
	 * EVERY exit path so the amp is never left enabled. The amp-off is an
	 * I2C write to the PMIC and can fail on the shared bus; log if it does.
	 */
	if (gpio_pin_set_dt(&amp_gpio, 0) < 0) {
		LOG_ERR("amp OFF failed; speaker may remain enabled");
	}
	audio_codec_stop_output(codec_dev);
}

/* Capture scratch + the latest mic level. The capture thread (below) is the only
 * active user of these on hardware (audio_loopback() is a dormant bring-up
 * primitive); mic_rms_peak is WRITTEN by that thread and READ by the UI thread
 * via audio_mic_level()/_bars(), so it is volatile (a 16-bit aligned access is
 * atomic on this unicore SoC).
 */
static int16_t zero_block[BLOCK_FRAMES * AUDIO_CHANNELS];
static int16_t rx_buf[BLOCK_FRAMES * AUDIO_CHANNELS];
static int16_t mono_buf[BLOCK_FRAMES];
static volatile uint16_t mic_rms_peak;

BUILD_ASSERT(sizeof(rx_buf) == BLOCK_SIZE, "rx_buf must hold exactly one I2S block");

/*
 * Bounded TX write for the loopback. The generic i2s_buf_write() allocates its
 * slab block with K_FOREVER, which would hang the caller forever if TX DMA
 * wedges; this mirrors it but with a finite alloc timeout so the loopback can
 * abort. i2s_write() only takes ownership of the block on success (the esp32
 * driver does not free on a queue-put failure), so on any failure we free it
 * here before returning the error.
 */
static int loop_tx(const int16_t *block)
{
	void *mem;
	int ret;

	ret = k_mem_slab_alloc(&tx_slab, &mem, K_MSEC(LOOP_IO_TIMEOUT_MS));
	if (ret < 0) {
		return ret;
	}
	memcpy(mem, block, BLOCK_SIZE);
	ret = i2s_write(i2s_dev, mem, BLOCK_SIZE);
	if (ret < 0) {
		k_mem_slab_free(&tx_slab, mem);
	}
	return ret;
}

/*
 * Read one captured I2S block, reduce it to a mono RMS/peak on AUDIO_MIC_SLOT,
 * print it (with the block index) and update the peak-hold. Returns 0, or a
 * negative errno if the bounded read failed so the caller can abort instead of
 * spinning on a wedged clock. When probe is set, also logs BOTH slots' RMS once
 * as an HW-016 aid: it makes a wrong AUDIO_MIC_SLOT obvious (silent slot vs the
 * one actually carrying the mono ADC) without a reflash.
 */
static int capture_report(const char *tag, uint32_t idx, bool probe)
{
	size_t size = sizeof(rx_buf);
	size_t frames;
	uint16_t rms;
	uint16_t peak;
	int ret;

	ret = i2s_buf_read(i2s_dev, rx_buf, &size);
	if (ret < 0) {
		LOG_WRN("i2s_buf_read(%s) failed (%d)", tag, ret);
		return ret;
	}

	/* Defensive: never let a returned size overrun mono_buf. */
	if (size > sizeof(rx_buf)) {
		size = sizeof(rx_buf);
	}
	frames = size / AUDIO_FRAME_BYTES;

	audio_deinterleave(rx_buf, frames, AUDIO_MIC_SLOT, mono_buf);
	rms = audio_rms_i16(mono_buf, frames);
	peak = audio_peak_i16(mono_buf, frames);
	if (rms > mic_rms_peak) {
		mic_rms_peak = rms;
	}
	printk("MIC %s[%u] rms=%u peak=%u\n", tag, idx, rms, peak);

	if (probe) {
		uint16_t rms0;
		uint16_t rms1;

		audio_deinterleave(rx_buf, frames, 0U, mono_buf);
		rms0 = audio_rms_i16(mono_buf, frames);
		audio_deinterleave(rx_buf, frames, 1U, mono_buf);
		rms1 = audio_rms_i16(mono_buf, frames);
		printk("MIC slot-probe: slot0 rms=%u slot1 rms=%u (using slot %u)\n",
		       rms0, rms1, (unsigned int)AUDIO_MIC_SLOT);
	}

	return 0;
}

void audio_loopback(void)
{
	int ret;

	if (!ready) {
		return;
	}
	if (!tone_ready) {
		tone_block_fill();
	}

	mic_rms_peak = 0U;

	/*
	 * Return both directions to READY first. A prior run that aborted on a
	 * wedged clock can leave the device in ERROR/STOPPING, and START is only
	 * valid from READY; DROP also frees any blocks still queued from a failed
	 * pre-queue, so this run always starts clean (without it a single fault
	 * would leave the loopback dead until reboot). Harmless when already idle.
	 */
	(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_DROP);

	audio_codec_start_output(codec_dev);

	/* Pre-queue two silent TX blocks so the shared clock starts cleanly. */
	for (uint32_t i = 0; i < 2U; i++) {
		if (loop_tx(zero_block) < 0) {
			LOG_ERR("loopback prequeue failed");
			goto stop;
		}
	}

	ret = i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_START);
	if (ret < 0) {
		LOG_ERR("i2s_trigger(BOTH START) failed (%d)", ret);
		goto stop;
	}

	/*
	 * Each phase alternates one TX write with one RX read so TX and RX advance
	 * in lockstep off the shared clock (neither starves). TX must keep streaming
	 * (silent blocks during the quiet phases) or the shared clock stops and RX
	 * stalls. Every I/O is bounded (LOOP_IO_TIMEOUT_MS); on the first failure we
	 * abort to stop:, which cuts the amp - so a wedged clock can never spin with
	 * the speaker energised. The amp is on only during the beep.
	 */
	/* Phase A: baseline silence -> low RMS floor. */
	for (uint32_t b = 0; b < LOOP_SIL_BLOCKS; b++) {
		if (loop_tx(zero_block) < 0 || capture_report("SIL", b, false) < 0) {
			goto stop;
		}
	}

	/* Phase B: 440 Hz beep -> RMS should spike (the mic hears the speaker). */
	k_msleep(AMP_SETTLE_MS);
	(void)gpio_pin_set_dt(&amp_gpio, 1);
	for (uint32_t b = 0; b < LOOP_BEEP_BLOCKS; b++) {
		/* Probe both slots once, on the first beep block (signal present). */
		if (loop_tx(tone_block) < 0 ||
		    capture_report("BEEP", b, b == 0U) < 0) {
			goto stop; /* stop: cuts the amp */
		}
	}
	(void)gpio_pin_set_dt(&amp_gpio, 0);

	/* Phase C: trailing silence -> RMS falls back to the floor. */
	for (uint32_t b = 0; b < LOOP_SIL_BLOCKS; b++) {
		if (loop_tx(zero_block) < 0 || capture_report("SIL", b, false) < 0) {
			goto stop;
		}
	}

	(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_DRAIN);
	printk("MIC loopback done: peak rms=%u\n", mic_rms_peak);

stop:
	if (gpio_pin_set_dt(&amp_gpio, 0) < 0) {
		LOG_ERR("amp OFF failed; speaker may remain enabled");
	}
	/* Flush both directions back to READY so the next run starts clean. */
	(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
	audio_codec_stop_output(codec_dev);
}

/*
 * PAGE_AUDIO live mic meter -- a dedicated CONTINUOUS-capture thread (issue #6).
 *
 * While capture_on is set (the AUDIO page is up), run ONE full-duplex session and
 * stream the ADC, updating mic_rms_peak with the peak RMS of each ~128 ms window
 * so audio_mic_bars()/audio_mic_level() track sound LIVE and fall when quiet.
 * This is the HW-016d-proven continuous-capture shape (one START, steady reads,
 * one DROP) -- NOT a per-render start/stop, which corrupted the very next SPI
 * display write and wedged I2S on hardware (HW-016e). The UI thread only READS
 * mic_rms_peak and NEVER touches I2S, so redrawing the meter can't break the
 * display from the UI side. Gated to the page (one START on enter, one DROP on
 * leave -- bounded churn, like the HW-013-proven audio_loopback). Any bounded-I/O
 * failure breaks to a DROP-only stop (skips DRAIN) so a wedged clock can't hang.
 */
#define CAPTURE_STACK_SIZE    3072
#define CAPTURE_PRIORITY      7
#define CAPTURE_WINDOW_BLOCKS 8U /* publish the level every ~128 ms */

static volatile bool capture_on;

void audio_capture_set(bool on)
{
	capture_on = on;
}

static void audio_capture_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		uint16_t wpeak = 0U;
		uint32_t wc = 0U;

		if (!ready || !capture_on) {
			mic_rms_peak = 0U;
			k_msleep(50);
			continue;
		}

		/* Start ONE full-duplex session (amp stays OFF for capture). */
		(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
		audio_codec_start_output(codec_dev);
		if (loop_tx(zero_block) < 0 || loop_tx(zero_block) < 0 ||
		    i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_START) < 0) {
			(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
			audio_codec_stop_output(codec_dev);
			k_msleep(50);
			continue;
		}

		/* Stream while still on the page. TX must keep feeding silence or
		 * the shared clock stops and RX stalls.
		 */
		while (ready && capture_on) {
			size_t size = sizeof(rx_buf);
			size_t frames;
			uint16_t rms;

			if (loop_tx(zero_block) < 0) {
				break;
			}
			if (i2s_buf_read(i2s_dev, rx_buf, &size) < 0) {
				break;
			}
			if (size > sizeof(rx_buf)) {
				size = sizeof(rx_buf);
			}
			frames = size / AUDIO_FRAME_BYTES;
			audio_deinterleave(rx_buf, frames, AUDIO_MIC_SLOT, mono_buf);
			rms = audio_rms_i16(mono_buf, frames);
			if (rms > wpeak) {
				wpeak = rms;
			}
			if (++wc >= CAPTURE_WINDOW_BLOCKS) {
				mic_rms_peak = wpeak;
				wpeak = 0U;
				wc = 0U;
			}
		}

		/* Stop: DROP only (skip DRAIN so a wedged clock can't hang), mute
		 * the DAC, and clear the level for the next visit.
		 */
		(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
		audio_codec_stop_output(codec_dev);
		mic_rms_peak = 0U;
	}
}

K_THREAD_DEFINE(audio_capture_tid, CAPTURE_STACK_SIZE, audio_capture_thread,
		NULL, NULL, NULL, CAPTURE_PRIORITY, 0, 0);

uint16_t audio_mic_level(void)
{
	return mic_rms_peak;
}

uint8_t audio_mic_bars(uint16_t level)
{
	return audio_level_bars(level, AUDIO_MIC_FULL, 4U);
}

#endif /* CONFIG_APP_AUDIO */
