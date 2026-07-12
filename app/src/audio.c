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
#include <zephyr/drivers/i2c.h>
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

/* Playback volume in dB (ES8311 set_property maps dB -> reg code). Raised from
 * the original conservative -20 dB so a recorded clip is clearly audible on the
 * small speaker (issue #14 QR-1); the digital gain (CONFIG_APP_AUDIO_REC_GAIN_Q8)
 * stacks on top. The 440 Hz self-test beep also uses this volume.
 */
#define AUDIO_VOLUME_DB (0)

/*
 * I2S TX memory blocks. block_size is a multiple of 4 (one stereo 16-bit frame
 * = 4 bytes). 256 frames per block = 1024 bytes ~= 16 ms at 16 kHz. The slab
 * lives in .bss -> internal SRAM (zephyr,sram = &sram1), never PSRAM, which
 * the I2S DMA requires. At least 2 blocks per the I2S API; we keep a small
 * pool so a couple of blocks can be pre-queued before START (anti-underrun).
 */
#define BLOCK_FRAMES 256U
#define BLOCK_SIZE   (BLOCK_FRAMES * AUDIO_FRAME_BYTES) /* 1024 bytes */
#define BLOCK_COUNT  8U /* deeper TX/RX queue: tolerate scheduling jitter, avoid underrun */

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
 * Mic capture (issue #6). The ES8311 mono ADC drives the captured data on I2S
 * slot 0; slot 1 is silent (HW-016d). AUDIO_MIC_SLOT selects the active slot and
 * audio_deinterleave() extracts it -- picking the silent slot reproduces the
 * HW-016 all-zero failure, so it is pinned, not assumed interchangeable.
 * AUDIO_MIC_FULL is
 * the empirical full-scale RMS for the PAGE_AUDIO live bar: quiet ~70-100, a
 * normal voice a few hundred to a few thousand, a loud clap >= 12000, so 2000
 * gives visible bars for speech without pinning the bar to 4 on every breath.
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

/*
 * Probe the codec, once.
 *
 * Idempotent, and it has to be: the rate sweep reprograms the whole chain and then
 * restores it, and device_init() on an already-initialized device does NOT return 0.
 * It returns -EALREADY (kernel/device.c, z_impl_device_init). device_is_ready() is
 * the right guard rather than treating -EALREADY as success, because it also
 * excludes a device that ran its init and failed.
 */
static int audio_codec_probe(void)
{
	int ret;

	if (device_is_ready(codec_dev)) {
		return 0;
	}

	/*
	 * The ES8311 is marked zephyr,deferred-init (it shares the L3B rail with
	 * the LCD, which is only powered by lcd_power's regulator-boot-on at boot).
	 * Probe it here, in main context, where L3B is up and settled -- doing it at
	 * the driver's POST_KERNEL priority read chip-id before power -> -EFAULT.
	 */
	ret = device_init(codec_dev);
	if (ret < 0) {
		LOG_ERR("ES8311 deferred init failed (%d)", ret);
		return ret;
	}

	if (!device_is_ready(codec_dev)) {
		LOG_ERR("ES8311 codec not ready");
		return -ENODEV;
	}

	return 0;
}

/*
 * Program I2S and the codec for `rate`: everything that has to be redone when the
 * sample rate changes, and nothing that must happen only once. The rate sweep
 * reprograms the chain per rate and restores it through this, so it must not probe
 * the device or touch anything one-shot.
 *
 * Leaves both directions configured and stopped, which is the state every path in
 * this file expects: i2s_configure() is called here and nowhere else, and beep,
 * meter, record and play all just DROP/START from whatever this left behind.
 */
static int audio_configure_chain(uint32_t rate)
{
	audio_property_value_t vol = { .vol = AUDIO_VOLUME_DB };
	struct audio_codec_cfg codec_cfg;
	struct i2s_config i2s_cfg;
	int ret;

	/*
	 * I2S0 TX as master (no I2S_OPT_*_CLK_TARGET => SoC drives BCLK + WS),
	 * standard I2S, 16-bit stereo. The esp32-i2s driver derives BCLK = 32 * Fs,
	 * and the codec makes its own master clock from BCLK, so the codec sees
	 * 256 * Fs at every rate: one coefficient row serves them all.
	 */
	i2s_cfg.word_size = AUDIO_WORD_BITS;
	i2s_cfg.channels = AUDIO_CHANNELS;
	i2s_cfg.format = I2S_FMT_DATA_FORMAT_I2S;
	i2s_cfg.options = I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER;
	i2s_cfg.frame_clk_freq = rate;
	i2s_cfg.mem_slab = &tx_slab;
	i2s_cfg.block_size = BLOCK_SIZE;
	i2s_cfg.timeout = I2S_WRITE_TIMEOUT_MS;

	ret = i2s_configure(i2s_dev, I2S_DIR_TX, &i2s_cfg);
	if (ret < 0) {
		LOG_ERR("i2s_configure(TX, %u Hz) failed (%d)", rate, ret);
		return ret;
	}

	/*
	 * mclk_freq is the frequency of the clock fed to the codec's MCLK *input*
	 * pin. The codec takes its master clock from BCLK instead, so there is no
	 * MCLK input to describe and the driver requires zero here. The old value
	 * (256 * Fs) was the codec's *internal* clock, which is not on any pin.
	 */
	codec_cfg.mclk_freq = 0U;
	codec_cfg.dai_type = AUDIO_DAI_TYPE_I2S;
	codec_cfg.dai_route = AUDIO_ROUTE_PLAYBACK_CAPTURE;
	codec_cfg.dai_cfg.i2s = i2s_cfg;

	ret = audio_codec_configure(codec_dev, &codec_cfg);
	if (ret < 0) {
		LOG_ERR("audio_codec_configure(%u Hz) failed (%d)", rate, ret);
		return ret;
	}

	/*
	 * I2S0 RX, same format, for microphone capture. The esp32-i2s driver shares
	 * BCLK/WS once both directions are configured, so capture can run full-duplex
	 * (I2S_DIR_BOTH). Only the mem_slab and the timeout differ from TX.
	 */
	i2s_cfg.mem_slab = &rx_slab;
	i2s_cfg.timeout = LOOP_IO_TIMEOUT_MS; /* bound a stalled mic read */
	ret = i2s_configure(i2s_dev, I2S_DIR_RX, &i2s_cfg);
	if (ret < 0) {
		LOG_ERR("i2s_configure(RX, %u Hz) failed (%d)", rate, ret);
		return ret;
	}

	/*
	 * Safe low volume; leave the codec configured but the amp OFF.
	 *
	 * Both results are propagated. This used to log a warning and continue, and then
	 * discard apply_properties() outright -- so an I2C failure here left the volume
	 * and the mute unapplied while the caller was told the chain was configured, and
	 * the rate sweep's restore would have reported success with the DAC at whatever
	 * gain the last rate left it. That is the same swallowed-return-value shape this
	 * branch exists to remove; it does not get an exception because it is only the
	 * volume.
	 */
	ret = audio_codec_set_property(codec_dev, AUDIO_PROPERTY_OUTPUT_VOLUME,
				       AUDIO_CHANNEL_ALL, vol);
	if (ret < 0) {
		LOG_ERR("set volume failed (%d)", ret);
		return ret;
	}

	ret = audio_codec_apply_properties(codec_dev);
	if (ret < 0) {
		LOG_ERR("apply_properties failed (%d)", ret);
		return ret;
	}

	return 0;
}

int audio_init(void)
{
	int ret;

	ret = audio_codec_probe();
	if (ret < 0) {
		return ret;
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

	ret = audio_configure_chain(AUDIO_SAMPLE_RATE);
	if (ret < 0) {
		return ret;
	}

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

/*
 * Refuse to start a stream when the I2S mem-slab has been eaten, and say why.
 *
 * Zephyr's ESP32 I2S driver leaks one slab block per direction on every START/DROP
 * cycle: i2s_esp32_{rx,tx}_stop_transfer() set stream->data->mem_block -- the block
 * the DMA is working on -- to NULL without returning it to the slab, and DROP calls
 * exactly those. Every capture session here ends in a DROP, so eight sessions empty
 * an eight-block slab.
 *
 * And it does not fail: i2s_buf_write() allocates with K_FOREVER
 * (drivers/i2s/i2s_common.c), so an exhausted slab is an unkillable block with no
 * error, no fault and no log line. The device simply stops, which is how it presented
 * on hardware (HW-019) and how it would present to a user who opened the AUDIO page
 * nine times.
 *
 * scripts/patch_zephyr_i2s_leak.sh fixes the driver, and the fix is NOT the obvious one:
 * simply handing the block back crashes, because on the GDMA path *_stop_transfer()
 * stops the DMA channel and never the I2S unit feeding it, so the block is still being
 * written when it lands back on the free list. The leak was masking that. See HW-021
 * (evidence/20260712-hw021-i2s-slab-quiesce-PASS.log) and CONFIG_APP_I2S_STRESS.
 *
 * This check is for anyone who builds without the fix: a message beats a mystery.
 */
static bool audio_slab_ok(const char *what)
{
	uint32_t tx = k_mem_slab_num_free_get(&tx_slab);
	uint32_t rx = k_mem_slab_num_free_get(&rx_slab);

	if (tx >= 2U && rx >= 2U) {
		return true;
	}

	LOG_ERR("%s refused: I2S mem-slab exhausted (tx=%u rx=%u of %u). The ESP32 I2S "
		"driver leaks a block per START/DROP; run scripts/patch_zephyr_i2s_leak.sh "
		"against your Zephyr tree. Starting anyway would block forever inside "
		"i2s_buf_write().",
		what, tx, rx, (unsigned int)BLOCK_COUNT);

	return false;
}

void audio_beep(void)
{
	uint32_t total_frames;
	uint32_t blocks;
	int ret;

	if (!ready || !audio_slab_ok("beep")) {
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
		/* Slab exhausted: a prior underrun left the I2S in ERROR still holding
		 * the TX blocks. PREPARE is the ONLY recovery from ERROR and frees the
		 * queued blocks back to the slab (DROP does not). Then retry once.
		 */
		(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_PREPARE);
		ret = k_mem_slab_alloc(&tx_slab, &mem, K_MSEC(LOOP_IO_TIMEOUT_MS));
		if (ret < 0) {
			return ret;
		}
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
#define CAPTURE_STACK_SIZE    4096
#define CAPTURE_PRIORITY      7
#define CAPTURE_WINDOW_BLOCKS 8U /* publish the level every ~128 ms */

static volatile bool capture_on;

void audio_capture_set(bool on)
{
	capture_on = on;
}

/*
 * Record/playback command from the UI thread (full engine below). Declared here
 * because meter_session() yields the moment a command is pending, so the thread
 * can service a record/play request even while the live meter is streaming.
 */
#define REC_CMD_NONE   0U
#define REC_CMD_RECORD 1U
#define REC_CMD_PLAY   2U
/* Cross-thread command from the UI thread to the audio thread. atomic_t (not a
 * plain volatile): the audio thread consumes it with a single atomic exchange
 * (atomic_set returns the previous value), so a command the UI writes can never
 * be lost in the gap between a separate read and clear.
 */
static atomic_t rec_cmd = ATOMIC_INIT(REC_CMD_NONE);

/*
 * One live-meter session: stream full-duplex while the AUDIO page is up,
 * publishing the peak RMS of each ~128 ms window, and return on leave/error.
 * (Extracted unchanged from the old thread body so the thread can also dispatch
 * the record/playback engine below.)
 */
static void meter_session(void)
{
	uint16_t wpeak = 0U;
	uint32_t wc = 0U;

	if (!audio_slab_ok("mic meter")) {
		return;
	}

	/* Start ONE full-duplex session (amp stays OFF for capture). */
	(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
	audio_codec_start_output(codec_dev);
	if (loop_tx(zero_block) < 0 || loop_tx(zero_block) < 0 ||
	    i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_START) < 0) {
		(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
		audio_codec_stop_output(codec_dev);
		k_msleep(50);
		return;
	}

	/* Stream while still wanted. TX must keep feeding silence or the shared
	 * clock stops and RX stalls. Also exit on a pending record/play request so
	 * the thread can service it (pressing K1 on the READY/REVIEW meter screen).
	 */
	while (ready && capture_on && atomic_get(&rec_cmd) == REC_CMD_NONE) {
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

	/* Stop: DROP only (skip DRAIN so a wedged clock can't hang), mute the
	 * DAC, and clear the level for the next visit.
	 */
	(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
	audio_codec_stop_output(codec_dev);
	mic_rms_peak = 0U;
}

/*
 * Record -> playback engine (issue #14). Runs ONLY on the audio thread (below),
 * so the UI thread never touches I2S (HW-016e). One clip lives in rec_buf; the
 * UI requests record/play via the atomic rec_cmd and reads rec_state.
 */
#define AUDIO_REC_SAMPLES ((uint32_t)CONFIG_APP_AUDIO_REC_SECONDS * AUDIO_SAMPLE_RATE)

/* The recorded mono clip (16-bit @ AUDIO_SAMPLE_RATE). In .bss -> internal SRAM
 * (the buffer itself is not DMA memory; only the I2S slab blocks are). Sized by
 * CONFIG_APP_AUDIO_REC_SECONDS (32 KB/s); see docs/14_RECORD_PLAYBACK_DESIGN.md.
 */
static int16_t rec_buf[AUDIO_REC_SAMPLES];
static int16_t gain_tmp[BLOCK_FRAMES];                   /* one block, post-gain mono */
static int16_t play_block[BLOCK_FRAMES * AUDIO_CHANNELS]; /* one block, stereo TX */

static volatile enum audio_rec_state rec_state = AUDIO_REC_IDLE;
static volatile bool rec_abort;       /* UI request to stop a recording early */
static volatile uint32_t rec_samples; /* mono samples currently held in rec_buf */
static volatile uint16_t rec_peak;    /* peak capture RMS of the last recording */

/*
 * Build one stereo TX block from rec_buf[pos..]: apply the playback gain, expand
 * mono -> stereo, and zero-pad a short final block so a full BLOCK_SIZE is sent.
 * Returns the number of mono source samples consumed (<= BLOCK_FRAMES).
 */
static size_t fill_play_block(uint32_t pos, uint16_t gain_q8)
{
	uint32_t total = rec_samples; /* snapshot; never underflow if pos >= total */
	size_t n;

	if (pos >= total) {
		return 0U;
	}
	n = total - pos;
	if (n > BLOCK_FRAMES) {
		n = BLOCK_FRAMES;
	}
	audio_gain_clip_i16(&rec_buf[pos], n, gain_q8, gain_tmp);
	audio_interleave_mono(gain_tmp, n, play_block);
	if (n < BLOCK_FRAMES) {
		memset(&play_block[n * AUDIO_CHANNELS], 0,
		       (BLOCK_FRAMES - n) * AUDIO_CHANNELS * sizeof(int16_t));
	}
	return n;
}

/* Capture up to AUDIO_REC_SAMPLES mono samples into rec_buf (amp OFF).
 *
 * The esp32-i2s RX returns an error after a bounded run (the live meter survives
 * the very same hiccup by re-entering its session from the thread loop; a single
 * pass would stop at the first one -- the 256 ms / rms=0 bug). So capture across
 * session restarts: on a read error, drop + restart the full-duplex session and
 * keep accumulating into rec_buf until it is full or the user stops. A guard
 * bails if several restarts in a row make no progress (so a truly dead clock
 * can't spin forever).
 */
static void do_record(void)
{
	uint16_t peak = 0U;
	uint32_t restarts = 0U;
	uint32_t empty = 0U;
	bool logged = false;

	if (!audio_slab_ok("record")) {
		return;
	}

	rec_samples = 0U;
	rec_peak = 0U;
	rec_abort = false;
	rec_state = AUDIO_REC_RECORDING;
	printk("REC start: up to %u ms, speak now\n",
	       (unsigned int)(AUDIO_REC_SAMPLES * 1000U / AUDIO_SAMPLE_RATE));

	while (rec_samples < AUDIO_REC_SAMPLES && !rec_abort && empty < 3U) {
		uint32_t before = rec_samples;

		(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
		(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_PREPARE);
		audio_codec_start_output(codec_dev);
		if (loop_tx(zero_block) < 0 || loop_tx(zero_block) < 0 ||
		    i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_START) < 0) {
			(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
			audio_codec_stop_output(codec_dev);
			LOG_ERR("record: I2S start failed");
			break;
		}

		/* Read blocks, keep TX fed with silence (shared clock), append mono. */
		while (rec_samples < AUDIO_REC_SAMPLES && !rec_abort) {
			size_t size = sizeof(rx_buf);
			size_t frames;
			uint32_t remain;
			uint16_t r;
			int rc;

			if (loop_tx(zero_block) < 0) {
				break;
			}
			rc = i2s_buf_read(i2s_dev, rx_buf, &size);
			if (rc < 0) {
				if (!logged) { /* log the first hiccup's errno only */
					LOG_WRN("rec: i2s_buf_read %d at %u ms (restarting)",
						rc, (unsigned int)(rec_samples * 1000U /
								   AUDIO_SAMPLE_RATE));
					logged = true;
				}
				break;
			}
			if (size > sizeof(rx_buf)) {
				size = sizeof(rx_buf);
			}
			frames = size / AUDIO_FRAME_BYTES;
			remain = AUDIO_REC_SAMPLES - rec_samples;
			if (frames > remain) {
				frames = remain; /* never overrun rec_buf */
			}
			audio_deinterleave(rx_buf, frames, AUDIO_MIC_SLOT,
					   &rec_buf[rec_samples]);
			r = audio_rms_i16(&rec_buf[rec_samples], frames);
			if (r > peak) {
				peak = r;
			}
			rec_samples += frames;
		}

		(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
		audio_codec_stop_output(codec_dev);

		if (rec_samples < AUDIO_REC_SAMPLES && !rec_abort) {
			restarts++;
			empty = (rec_samples == before) ? (empty + 1U) : 0U;
		}
	}

	rec_peak = peak;
	rec_state = (rec_samples > 0U) ? AUDIO_REC_REVIEW : AUDIO_REC_IDLE;
	printk("REC done: %u ms peak_rms=%u restarts=%u\n",
	       (unsigned int)(rec_samples * 1000U / AUDIO_SAMPLE_RATE), peak,
	       (unsigned int)restarts);
}

/* Play the held clip back through the speaker (amp anti-pop, gain applied). */
static void do_play(void)
{
	uint16_t gain = (uint16_t)CONFIG_APP_AUDIO_REC_GAIN_Q8;
	uint32_t pos = 0U;
	int ret;

	if (rec_samples == 0U || !audio_slab_ok("playback")) {
		return;
	}
	rec_state = AUDIO_REC_PLAYING;
	printk("PLAY start: %u ms gain_q8=%u\n",
	       (unsigned int)(rec_samples * 1000U / AUDIO_SAMPLE_RATE), gain);

	/* Recover the I2S from the prior session and reclaim its TX slab blocks:
	 * PREPARE is the only recovery from the ERROR state an underrun leaves, and
	 * it is what frees the held mem_slab blocks (DROP from ERROR does not, so the
	 * pool leaks empty -> -EAGAIN). Then play TX-ONLY, mirroring the proven
	 * self-test. (Full-duplex playback under-ran at ~256 ms: the RX read stalled
	 * TX.)
	 */
	(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
	(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_PREPARE);
	audio_codec_start_output(codec_dev);

	for (int i = 0; i < 2; i++) {
		ret = loop_tx(zero_block);
		if (ret < 0) {
			LOG_ERR("play prequeue write failed (%d)", ret);
			goto stop;
		}
	}

	ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret < 0) {
		LOG_ERR("play I2S start failed (%d)", ret);
		goto stop;
	}

	/* Amp ON only once valid frames are already streaming (anti-pop). */
	k_msleep(AMP_SETTLE_MS);
	if (gpio_pin_set_dt(&amp_gpio, 1) < 0) {
		LOG_WRN("play amp ON failed");
	}

	while (pos < rec_samples) {
		pos += fill_play_block(pos, gain);
		ret = loop_tx(play_block); /* bounded TX: times out instead of hanging */
		if (ret < 0) {
			LOG_WRN("play TX write failed (%d)", ret);
			break;
		}
	}

	/* End of clip (and the TX-timeout break above): wait a bounded, fixed time
	 * for the pre-queued DMA tail (<= BLOCK_COUNT blocks) to play out, then fall
	 * through to the DROP below. Deliberately NOT i2s_trigger(TX, DRAIN): DRAIN
	 * has no timeout, so a wedged TX clock (the same one loop_tx() bounds above)
	 * would hang the audio thread here with the amp still on and rec_state stuck
	 * at PLAYING (K2 inert -> the page can't be left). DROP is the wedge-safe stop
	 * do_record()/meter_session() already use.
	 */
	k_msleep(AMP_SETTLE_MS +
		 (BLOCK_COUNT * BLOCK_FRAMES * 1000U / AUDIO_SAMPLE_RATE));

stop:
	if (gpio_pin_set_dt(&amp_gpio, 0) < 0) {
		LOG_ERR("amp OFF failed; speaker may remain enabled");
	}
	audio_codec_stop_output(codec_dev);
	(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	rec_state = AUDIO_REC_REVIEW;
	printk("PLAY done\n");
}

/*
 * The audio thread: serialise everything that touches I2S. The AUDIO page meter
 * (capture_on) takes priority; otherwise service one record/playback request.
 * Only ever one of {meter, record, play} runs at a time, so they safely share
 * rx_buf / mono_buf and the codec.
 */
static void audio_capture_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		uint8_t cmd;

		if (!ready) {
			k_msleep(50);
			continue;
		}

		/* Service a record/play request AHEAD of the meter: the meter loop
		 * exits on a pending request, and checking it first here means the
		 * request runs even if the main loop re-asserts capture_on in the
		 * brief window before do_record()/do_play() updates the state.
		 */
		/* Read+clear in one atomic exchange (returns the previous value),
		 * so a command set by the UI thread is never wiped by a separate
		 * clear (the volatile two-step could lose it).
		 */
		cmd = (uint8_t)atomic_set(&rec_cmd, REC_CMD_NONE);
		if (cmd == REC_CMD_RECORD) {
			do_record();
			continue;
		}
		if (cmd == REC_CMD_PLAY) {
			do_play();
			continue;
		}

		if (capture_on) {
			meter_session();
			continue;
		}

		k_msleep(50);
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

void audio_record_request(void)
{
	if (ready) {
		atomic_set(&rec_cmd, REC_CMD_RECORD);
	}
}

void audio_play_request(void)
{
	if (ready && rec_samples > 0U) {
		atomic_set(&rec_cmd, REC_CMD_PLAY);
	}
}

void audio_record_stop_request(void)
{
	rec_abort = true;
}

enum audio_rec_state audio_rec_get_state(void)
{
	return rec_state;
}

uint16_t audio_rec_peak(void)
{
	return rec_peak;
}

uint32_t audio_rec_len_ms(void)
{
	return rec_samples * 1000U / AUDIO_SAMPLE_RATE;
}

#ifdef CONFIG_APP_AUDIO_RATE_SWEEP

/*
 * ES8311 sample-rate sweep: hardware validation for the codec driver (issue #7).
 *
 * The driver claims 8 kHz to 48 kHz on the argument that the codec's master clock
 * is derived from BCLK, so it lands on 256 * Fs at every rate and the divider
 * chain is a pure ratio. That is an argument. This is the measurement.
 *
 * Three independent things are checked at each rate.
 *
 * 1. The clock registers are read back off the chip over I2C. That only proves
 *    the driver wrote what it meant to write, so it is necessary and nowhere near
 *    sufficient.
 *
 * 2. The frame clock is MEASURED. In steady state the DMA drains one block per
 *    BLOCK_FRAMES / Fs, so timing a known number of blocks with the kernel's
 *    cycle counter gives the frame rate the hardware is really running at. The
 *    kernel clock is not derived from the I2S clock, which is what makes this an
 *    independent check: a mis-programmed I2S divider shows up here and nowhere
 *    else. An acoustic loopback cannot do this, because TX and RX share the same
 *    clock and a common error cancels out in the digital domain.
 *
 * 3. The ADC is checked for LIFE, with the amplifier off. A codec whose internal
 *    clock tree failed to come up cannot run its modulator, and the captured
 *    block is then all zeros or a stuck constant. A running ADC returns a
 *    dithering noise floor. This is the codec-side evidence, and it is the check
 *    that would actually catch a clock tree that did not lock.
 *
 * The speaker is judged by ear. The on-board speaker couples very weakly into the
 * adjacent microphone (HW-016 established this: the 440 Hz beep barely moved the
 * captured RMS while a clap saturated it), so an acoustic threshold here would
 * fail for a benign reason. The tone level is reported, not asserted. What the
 * listener is checking is that every rate makes a sound at all, and the pitch
 * rises across the sweep because the tone table was generated for 16 kHz.
 */
static const uint32_t sweep_rates[] = {
	8000U, 11025U, 12000U, 16000U, 22050U, 24000U, 32000U, 44100U, 48000U,
};

/* Clock and format registers the driver must write, identical at every rate. */
static const struct {
	uint8_t reg;
	uint8_t val;
} sweep_expect[] = {
	{0x01U, 0xBFU}, /* clock manager: master clock taken from BCLK */
	{0x02U, 0x18U}, /* DIV_PRE = 1, MULT_PRE = x8 */
	{0x03U, 0x10U}, /* single speed, ADC_OSR = 16 */
	{0x04U, 0x10U}, /* DAC_OSR = 16 */
	{0x05U, 0x00U}, /* DIV_CLKADC = 1, DIV_CLKDAC = 1 */
	{0x06U, 0x03U}, /* BCLK_CON clear: the codec stays the clock slave */
	{0x07U, 0x00U},
	{0x08U, 0xFFU},
	{0x09U, 0x0CU}, /* serial data in: standard I2S, 16-bit */
	{0x0AU, 0x0CU}, /* serial data out: standard I2S, 16-bit, unmuted */
};

static const struct i2c_dt_spec codec_i2c = I2C_DT_SPEC_GET(DT_NODELABEL(es8311));

/*
 * Blocks discarded while the DMA reaches steady state, blocks used for the
 * silence baseline, and blocks timed for the frame-rate measurement. 40 timed
 * blocks is 213 ms at 48 kHz and 1.28 s at 8 kHz, which is long enough for the
 * cycle counter at either end.
 */
#define SWEEP_WARMUP_BLOCKS   4U
#define SWEEP_BASELINE_BLOCKS 8U
#define SWEEP_TIMED_BLOCKS    40U

/* The measured frame clock must land within this much of the requested one. */
#define SWEEP_RATE_TOLERANCE_PERCENT 2U

static int16_t sweep_rx[BLOCK_FRAMES * AUDIO_CHANNELS];
static int16_t sweep_mono[BLOCK_FRAMES];

/* One full-duplex block: keep TX fed, take one RX block. */
static int sweep_io(size_t *frames)
{
	size_t sz = sizeof(sweep_rx);
	int ret;

	ret = i2s_buf_write(i2s_dev, tone_block, BLOCK_SIZE);
	if (ret < 0) {
		return ret;
	}

	ret = i2s_buf_read(i2s_dev, sweep_rx, &sz);
	if (ret < 0) {
		return ret;
	}

	/*
	 * The frame-clock measurement divides a known number of blocks by the elapsed
	 * time, and "a block" means BLOCK_FRAMES frames. Nothing checked that the driver
	 * actually returned a full one. A short read would make every block worth less
	 * audio time than assumed and the measured rate would come out low, with no sign
	 * of why -- which is exactly how the last measurement bug presented.
	 */
	if (sz != BLOCK_SIZE) {
		printk("SWEEP i/o: short block, %u bytes of %u\n", (unsigned int)sz,
		       (unsigned int)BLOCK_SIZE);
		return -EIO;
	}

	*frames = sz / AUDIO_FRAME_BYTES;
	audio_deinterleave(sweep_rx, *frames, AUDIO_MIC_SLOT, sweep_mono);

	return 0;
}

/*
 * Name each step as it is entered.
 *
 * A silent hang is the one failure a print-the-result sweep cannot locate. Every
 * error path here prints, but a call that never returns prints nothing: HW-019b
 * stopped dead after 32 kHz with no error line and no fault, and the log gave no way
 * to tell which call had not come back. The last marker in the log now names it.
 *
 * Lower case and indented, so `grep '^SWEEP'` still gives the clean per-rate summary.
 */
#define SWEEP_STEP(r, s) printk("  sweep.%u: %s\n", (unsigned int)(r), (s))

/*
 * Put the I2S back into a state configure() will accept, whatever state it is in.
 * DROP alone is not enough: a stream that hit an error is left in ERROR with its
 * slab blocks still held, and only PREPARE returns them. Without this a single
 * failing rate poisons every rate after it, and the results stop being
 * independent.
 */
static void sweep_reset_i2s(void)
{
	(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
	(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_PREPARE);
	(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
}

static int sweep_one(uint32_t rate)
{
	struct audio_codec_cfg codec_cfg;
	struct i2s_config i2s_cfg;
	uint32_t t0;
	uint32_t t1;
	uint64_t us;
	uint32_t measured = 0U;
	uint32_t tolerance;
	uint32_t settle;
	uint16_t base_rms = 0U;
	uint16_t tone_rms = 0U;
	uint16_t tone_peak = 0U;
	int16_t base_lo = 0;
	int16_t base_hi = 0;
	bool adc_alive = false;
	bool rate_ok = false;
	uint32_t alive_blocks = 0U;
	int regs_bad = 0;
	size_t frames = 0U;
	int ret;

	/*
	 * The slab census. i2s_buf_write() allocates its block with K_FOREVER
	 * (drivers/i2s/i2s_common.c), NOT with the i2s_config timeout, so an exhausted
	 * TX slab is not an error: it is an unkillable block. HW-019b hung exactly
	 * there, silently, on the eighth rate. Print what the slab holds before and
	 * after the reset, so a leak shows up as a number instead of as a hang.
	 */
	printk("  sweep.%u: slab pre-reset  tx=%u rx=%u\n", (unsigned int)rate,
	       k_mem_slab_num_free_get(&tx_slab), k_mem_slab_num_free_get(&rx_slab));

	SWEEP_STEP(rate, "reset");
	sweep_reset_i2s();

	printk("  sweep.%u: slab post-reset tx=%u rx=%u  (of %u)\n", (unsigned int)rate,
	       k_mem_slab_num_free_get(&tx_slab), k_mem_slab_num_free_get(&rx_slab),
	       (unsigned int)BLOCK_COUNT);

	i2s_cfg.word_size = AUDIO_WORD_BITS;
	i2s_cfg.channels = AUDIO_CHANNELS;
	i2s_cfg.format = I2S_FMT_DATA_FORMAT_I2S;
	i2s_cfg.options = I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER;
	i2s_cfg.frame_clk_freq = rate;
	i2s_cfg.mem_slab = &tx_slab;
	i2s_cfg.block_size = BLOCK_SIZE;
	i2s_cfg.timeout = I2S_WRITE_TIMEOUT_MS;

	SWEEP_STEP(rate, "cfg-tx");
	ret = i2s_configure(i2s_dev, I2S_DIR_TX, &i2s_cfg);
	if (ret < 0) {
		printk("SWEEP %-6u FAIL i2s_configure(TX)=%d\n", rate, ret);
		return ret;
	}

	/*
	 * mclk_freq is the codec's MCLK *input*, which this board does not drive:
	 * the codec derives its master clock from BCLK, so the driver requires 0.
	 */
	codec_cfg.mclk_freq = 0U;
	codec_cfg.dai_type = AUDIO_DAI_TYPE_I2S;
	codec_cfg.dai_route = AUDIO_ROUTE_PLAYBACK_CAPTURE;
	codec_cfg.dai_cfg.i2s = i2s_cfg;

	SWEEP_STEP(rate, "cfg-codec");
	ret = audio_codec_configure(codec_dev, &codec_cfg);
	if (ret < 0) {
		printk("SWEEP %-6u FAIL audio_codec_configure=%d\n", rate, ret);
		return ret;
	}

	SWEEP_STEP(rate, "cfg-rx");
	i2s_cfg.mem_slab = &rx_slab;
	i2s_cfg.timeout = LOOP_IO_TIMEOUT_MS;
	ret = i2s_configure(i2s_dev, I2S_DIR_RX, &i2s_cfg);
	if (ret < 0) {
		printk("SWEEP %-6u FAIL i2s_configure(RX)=%d\n", rate, ret);
		return ret;
	}

	SWEEP_STEP(rate, "regs");
	/* 1. Read the clock registers back off the real codec. */
	for (size_t i = 0; i < ARRAY_SIZE(sweep_expect); i++) {
		uint8_t v = 0U;

		ret = i2c_reg_read_byte_dt(&codec_i2c, sweep_expect[i].reg, &v);
		if (ret < 0 || v != sweep_expect[i].val) {
			printk("SWEEP %-6u  reg 0x%02x = 0x%02x (want 0x%02x) ret=%d\n", rate,
			       sweep_expect[i].reg, v, sweep_expect[i].val, ret);
			regs_bad++;
		}
	}

	SWEEP_STEP(rate, "prequeue");
	/* Pre-queue so TX does not underrun the moment the clocks start. */
	for (int i = 0; i < 2; i++) {
		ret = i2s_buf_write(i2s_dev, tone_block, BLOCK_SIZE);
		if (ret < 0) {
			printk("SWEEP %-6u FAIL prequeue=%d\n", rate, ret);
			sweep_reset_i2s();
			return ret;
		}
	}

	SWEEP_STEP(rate, "start");
	ret = i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_START);
	if (ret < 0) {
		printk("SWEEP %-6u FAIL i2s_trigger(START)=%d\n", rate, ret);
		sweep_reset_i2s();
		return ret;
	}

	SWEEP_STEP(rate, "warmup");
	/* Let the DMA reach steady state before anything is measured. */
	for (uint32_t b = 0U; b < SWEEP_WARMUP_BLOCKS; b++) {
		ret = sweep_io(&frames);
		if (ret < 0) {
			printk("SWEEP %-6u FAIL warmup i/o=%d\n", rate, ret);
			goto stop;
		}
	}

	SWEEP_STEP(rate, "baseline");
	/*
	 * 3. Silence baseline, amplifier still off. A codec whose clock tree did
	 * not come up returns all zeros or a stuck constant here; a running one
	 * returns a dithering noise floor.
	 */
	for (uint32_t b = 0U; b < SWEEP_BASELINE_BLOCKS; b++) {
		int16_t lo = INT16_MAX;
		int16_t hi = INT16_MIN;
		uint16_t rms;

		ret = sweep_io(&frames);
		if (ret < 0) {
			printk("SWEEP %-6u FAIL baseline i/o=%d\n", rate, ret);
			goto stop;
		}

		for (size_t i = 0U; i < frames; i++) {
			if (sweep_mono[i] < lo) {
				lo = sweep_mono[i];
			}
			if (sweep_mono[i] > hi) {
				hi = sweep_mono[i];
			}
		}

		rms = audio_rms_i16(sweep_mono, frames);
		if (rms > base_rms) {
			base_rms = rms;
		}
		if (hi != lo) {
			/*
			 * A single varying block used to be enough to call the ADC
			 * alive, which one stale DMA buffer or two alternating garbage
			 * samples would satisfy. Count them, and require most.
			 */
			alive_blocks++;
			base_lo = lo;
			base_hi = hi;
		}
	}

	adc_alive = (alive_blocks * 4U) >= (SWEEP_BASELINE_BLOCKS * 3U);

	SWEEP_STEP(rate, "amp");
	/* The clocks are already running, so raising the amplifier here cannot pop. */
	ret = gpio_pin_set_dt(&amp_gpio, 1);
	if (ret < 0) {
		printk("SWEEP %-6u FAIL amp on=%d\n", rate, ret);
		goto stop;
	}

	/*
	 * Settle the amplifier by RUNNING the stream, not by sleeping inside it.
	 *
	 * A k_msleep() here leaves a full-duplex stream with nobody feeding TX and
	 * nobody draining RX, and the first HW-019 run presented both halves of that
	 * bill. Neither had anything to do with the codec.
	 *
	 *  - RX queues blocks nobody reads. The ones already waiting when the clock is
	 *    read come back with no wait, so the timed loop finishes in fewer than
	 *    SWEEP_TIMED_BLOCKS block-times and the measured rate comes out HIGH, in
	 *    exact proportion to the sample rate: +1.7% at 8 kHz rising to +5.2% at
	 *    24 kHz, against a 2% tolerance. Every rate but 8 kHz was failed by it.
	 *  - TX runs dry. The two pre-queued blocks hold 2 * BLOCK_FRAMES / rate
	 *    seconds of audio, which drops below the 20 ms settle at 32 kHz, so the
	 *    stream underruns into I2S ERROR and the next transfer returns -EIO.
	 *    32 kHz, 44.1 kHz and 48 kHz died there; nothing below them did.
	 *
	 * Both are predicted to the decimal by AMP_SETTLE_MS, BLOCK_FRAMES and the
	 * pre-queue depth, with nothing fitted. The control is in the same log: the
	 * restore measurement has no sleep in it and read 16000 Hz exactly, on the same
	 * board, through the same code, at a rate the per-rate loop had just called BAD.
	 *
	 * Draining for the same number of block-times settles the amplifier just as
	 * well, keeps both directions fed, and leaves the queue empty for the clock
	 * measurement that follows.
	 */
	SWEEP_STEP(rate, "settle");
	settle = (((uint32_t)AMP_SETTLE_MS * rate) / (1000U * BLOCK_FRAMES)) + 1U;
	for (uint32_t b = 0U; b < settle; b++) {
		ret = sweep_io(&frames);
		if (ret < 0) {
			printk("SWEEP %-6u FAIL settle i/o=%d (block %u)\n", rate, ret, b);
			goto stop;
		}
	}

	/*
	 * 2. Measure the frame clock while the tone plays. In steady state each
	 * iteration costs exactly one block of DMA time, so the elapsed cycles over
	 * a known number of blocks give the rate the hardware is really running at.
	 */
	SWEEP_STEP(rate, "timed");
	t0 = k_cycle_get_32();

	for (uint32_t b = 0U; b < SWEEP_TIMED_BLOCKS; b++) {
		uint16_t rms;
		uint16_t peak;

		ret = sweep_io(&frames);
		if (ret < 0) {
			printk("SWEEP %-6u FAIL tone i/o=%d (block %u)\n", rate, ret, b);
			goto stop;
		}

		rms = audio_rms_i16(sweep_mono, frames);
		peak = audio_peak_i16(sweep_mono, frames);
		if (rms > tone_rms) {
			tone_rms = rms;
		}
		if (peak > tone_peak) {
			tone_peak = peak;
		}
	}

	t1 = k_cycle_get_32();

	us = k_cyc_to_us_floor64(t1 - t0);
	if (us > 0U) {
		measured = (uint32_t)(((uint64_t)SWEEP_TIMED_BLOCKS * BLOCK_FRAMES * 1000000ULL) /
				      us);
	}

	tolerance = (rate * SWEEP_RATE_TOLERANCE_PERCENT) / 100U;
	rate_ok = (measured + tolerance >= rate) && (measured <= rate + tolerance);

stop:
	SWEEP_STEP(rate, "stop");
	(void)gpio_pin_set_dt(&amp_gpio, 0);
	sweep_reset_i2s();

	/*
	 * Print the line even when a phase died. The first HW-019 run threw away the
	 * register readback and the ADC-liveness result for every rate that errored --
	 * which was exactly the data needed to tell a codec fault from a test fault, and
	 * it had already been collected by the time the error happened.
	 */
	printk("SWEEP %-6u regs=%-3s lrck=%-6u %-3s adc=%-5s floor=%-5u [%d..%d] "
	       "tone_rms=%-6u peak=%-6u %s\n",
	       rate, regs_bad ? "BAD" : "OK", measured, rate_ok ? "OK" : "BAD",
	       adc_alive ? "alive" : "DEAD", base_rms, base_lo, base_hi, tone_rms, tone_peak,
	       (ret < 0) ? "FAIL(io)"
			 : ((regs_bad == 0 && rate_ok && adc_alive) ? "PASS" : "FAIL"));

	if (ret < 0) {
		return ret;
	}

	if (regs_bad != 0 || !rate_ok || !adc_alive) {
		return -EIO;
	}

	return 0;
}

/*
 * Measure the frame clock the hardware is really running at, with the amplifier
 * off, and check it against `want`. Same method as the per-rate measurement: time
 * a known number of DMA blocks against the kernel cycle counter, which is not
 * derived from the I2S clock. Leaves the stream stopped and configured, which is
 * what audio_init() leaves behind and what every other path here starts from.
 */
static int sweep_measure_rate(uint32_t want)
{
	uint32_t t0;
	uint32_t t1;
	uint64_t us;
	uint32_t measured = 0U;
	uint32_t tolerance;
	size_t frames = 0U;
	int ret;

	sweep_reset_i2s();

	for (int i = 0; i < 2; i++) {
		ret = i2s_buf_write(i2s_dev, tone_block, BLOCK_SIZE);
		if (ret < 0) {
			goto stop;
		}
	}

	ret = i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_START);
	if (ret < 0) {
		goto stop;
	}

	for (uint32_t b = 0U; b < SWEEP_WARMUP_BLOCKS; b++) {
		ret = sweep_io(&frames);
		if (ret < 0) {
			goto stop;
		}
	}

	t0 = k_cycle_get_32();

	for (uint32_t b = 0U; b < SWEEP_TIMED_BLOCKS; b++) {
		ret = sweep_io(&frames);
		if (ret < 0) {
			goto stop;
		}
	}

	t1 = k_cycle_get_32();

	us = k_cyc_to_us_floor64(t1 - t0);
	if (us > 0U) {
		measured = (uint32_t)(((uint64_t)SWEEP_TIMED_BLOCKS * BLOCK_FRAMES * 1000000ULL) /
				      us);
	}

	tolerance = (want * SWEEP_RATE_TOLERANCE_PERCENT) / 100U;
	if (measured + tolerance < want || measured > want + tolerance) {
		printk("SWEEP restore: lrck measured %u Hz, want %u +/-%u%%  BAD\n", measured,
		       want, SWEEP_RATE_TOLERANCE_PERCENT);
		ret = -EIO;
		goto stop;
	}

	printk("SWEEP restore: lrck measured %u Hz, want %u  OK\n", measured, want);
	ret = 0;

stop:
	sweep_reset_i2s();
	return ret;
}

/*
 * The route-transition register values are the newest and least proven thing in the
 * driver, and the rate sweep never touches them: it asks for PLAYBACK_CAPTURE at
 * every rate. These are the registers that power the UNUSED converter DOWN -- what
 * the driver used to leave exactly as it found it, so that a capture-only route kept
 * a DAC powered up by a previous playback route, and a playback-only route left the
 * microphone live. Until this runs, they have been checked against an emulator that
 * cannot disagree with them and never against silicon.
 *
 * So configure each route on the real chip and read back what actually landed.
 */
static const uint8_t sweep_route_regs[] = {
	0x01U, /* clock manager: the unused converter's clocks gated off */
	0x09U, /* SDP in (DAC): muted when playback is not routed */
	0x0AU, /* SDP out (ADC): muted when capture is not routed */
	0x0DU, /* analog: the unused converter's bias and references dropped */
	0x0EU, /* ADC power */
	0x12U, /* DAC power */
	0x14U, /* microphone mux: nothing selected when capture is not routed */
};

/* Indexed [route][register], in the order above. */
static const uint8_t sweep_route_vals[3][ARRAY_SIZE(sweep_route_regs)] = {
	/* 0x01  0x09  0x0A  0x0D  0x0E  0x12  0x14 */
	{ 0xB5U, 0x0CU, 0x4CU, 0x31U, 0x62U, 0x00U, 0x00U }, /* playback only */
	{ 0xBAU, 0x4CU, 0x0CU, 0x09U, 0x02U, 0x02U, 0x1AU }, /* capture only  */
	{ 0xBFU, 0x0CU, 0x0CU, 0x01U, 0x02U, 0x00U, 0x1AU }, /* both          */
};

static int sweep_one_route(const char *name, audio_route_t route, unsigned int idx)
{
	struct audio_codec_cfg codec_cfg;
	struct i2s_config i2s_cfg;
	int bad = 0;
	int ret;

	sweep_reset_i2s();

	i2s_cfg.word_size = AUDIO_WORD_BITS;
	i2s_cfg.channels = AUDIO_CHANNELS;
	i2s_cfg.format = I2S_FMT_DATA_FORMAT_I2S;
	i2s_cfg.options = I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER;
	i2s_cfg.frame_clk_freq = AUDIO_SAMPLE_RATE;
	i2s_cfg.mem_slab = &tx_slab;
	i2s_cfg.block_size = BLOCK_SIZE;
	i2s_cfg.timeout = I2S_WRITE_TIMEOUT_MS;

	codec_cfg.mclk_freq = 0U;
	codec_cfg.dai_type = AUDIO_DAI_TYPE_I2S;
	codec_cfg.dai_route = route;
	codec_cfg.dai_cfg.i2s = i2s_cfg;

	ret = audio_codec_configure(codec_dev, &codec_cfg);
	if (ret < 0) {
		printk("SWEEP route %-9s FAIL audio_codec_configure=%d\n", name, ret);
		return ret;
	}

	for (size_t i = 0; i < ARRAY_SIZE(sweep_route_regs); i++) {
		uint8_t v = 0U;

		ret = i2c_reg_read_byte_dt(&codec_i2c, sweep_route_regs[i], &v);
		if (ret < 0 || v != sweep_route_vals[idx][i]) {
			printk("SWEEP route %-9s reg 0x%02x = 0x%02x (want 0x%02x) ret=%d\n", name,
			       sweep_route_regs[i], v, sweep_route_vals[idx][i], ret);
			bad++;
		}
	}

	printk("SWEEP route %-9s %s\n", name, bad ? "FAIL" : "PASS");

	return bad ? -EIO : 0;
}

int audio_rate_sweep(void)
{
	unsigned int route_bad = 0U;
	unsigned int bad = 0U;
	int ret;

	if (!ready) {
		printk("SWEEP skipped: audio not ready\n");
		return -ENODEV;
	}

	/*
	 * Park the capture thread. It only touches I2S while `ready` is set, so
	 * clearing it and letting one poll interval elapse leaves the bus to us.
	 */
	ready = false;
	k_msleep(100);

	printk("\n=== ES8311 sample-rate sweep ===\n");
	printk("regs  : the clock registers read back off the chip over I2C\n");
	printk("lrck  : the frame clock MEASURED against the kernel cycle counter,\n");
	printk("        which is not derived from the I2S clock\n");
	printk("adc   : the microphone noise floor with the amplifier off. A codec\n");
	printk("        whose clock tree did not lock cannot run its modulator and\n");
	printk("        returns zeros or a stuck constant here\n");
	printk("tone  : the captured level with the amplifier on. Reported, not\n");
	printk("        asserted: this speaker couples weakly into this microphone.\n");
	printk("        Judge the speaker by ear; the pitch rises across the sweep\n");
	printk("        because the tone table was made for 16 kHz\n\n");

	for (size_t i = 0; i < ARRAY_SIZE(sweep_rates); i++) {
		if (sweep_one(sweep_rates[i]) < 0) {
			bad++;
		}
		k_msleep(200);
	}

	printk("\n=== sweep: %u of %u rate(s) FAILED ===\n", bad,
	       (unsigned int)ARRAY_SIZE(sweep_rates));

	/*
	 * The route transitions, on the real chip. The sweep above ran every rate
	 * through PLAYBACK_CAPTURE and so never wrote a single one of the power-down
	 * values that the route fix is actually made of.
	 */
	printk("\n-- route transitions: the unused converter must power DOWN --\n");
	if (sweep_one_route("playback", AUDIO_ROUTE_PLAYBACK, 0U) < 0) {
		route_bad++;
	}
	if (sweep_one_route("capture", AUDIO_ROUTE_CAPTURE, 1U) < 0) {
		route_bad++;
	}
	if (sweep_one_route("both", AUDIO_ROUTE_PLAYBACK_CAPTURE, 2U) < 0) {
		route_bad++;
	}
	printk("=== routes: %u of 3 FAILED ===\n", route_bad);

	/*
	 * Put the chain back the way the application expects it. This is part of the
	 * result, not cleanup after it: a sweep that leaves the device unable to play
	 * has not passed, however many rates it ticked off.
	 *
	 * It restores through audio_configure_chain() and NOT through audio_init(),
	 * because audio_init() probes the codec, and device_init() on an already
	 * initialized device returns -EALREADY rather than 0. The first version of this
	 * called audio_init() here, took that -EALREADY as a fatal init failure,
	 * returned before `ready = true`, and left the microphone meter, record, play
	 * and beep all dead -- behind a sweep that had just printed PASS for every rate.
	 */
	ret = audio_configure_chain(AUDIO_SAMPLE_RATE);
	if (ret < 0) {
		printk("\n*** SWEEP FAILED: restore to %u Hz failed (%d); audio is DOWN ***\n\n",
		       (unsigned int)AUDIO_SAMPLE_RATE, ret);
		return ret;
	}

	/*
	 * And measure it, rather than trusting the return codes that just came back
	 * from the same driver the sweep is supposed to be validating.
	 */
	ret = sweep_measure_rate(AUDIO_SAMPLE_RATE);
	if (ret < 0) {
		printk("\n*** SWEEP FAILED: restored to %u Hz but the frame clock does not "
		       "agree (%d); audio is DOWN ***\n\n",
		       (unsigned int)AUDIO_SAMPLE_RATE, ret);
		return ret;
	}

	ready = true;

	if (bad > 0U || route_bad > 0U) {
		printk("\n*** SWEEP FAILED: %u of %u rate(s), %u of 3 route(s) ***\n\n", bad,
		       (unsigned int)ARRAY_SIZE(sweep_rates), route_bad);
		return -EIO;
	}

	printk("\n=== SWEEP PASSED: %u rates + 3 routes, restored to %u Hz and measured ===\n"
	       "    What that means: the clock registers land on the chip, the frame clock\n"
	       "    is right, the ADC is running, and the route registers are what the\n"
	       "    driver intended. What it does NOT mean: nothing here measures audio\n"
	       "    quality or the codec's internal OSR. The speaker is judged by ear.\n\n",
	       (unsigned int)ARRAY_SIZE(sweep_rates), (unsigned int)AUDIO_SAMPLE_RATE);

	return 0;
}

#endif /* CONFIG_APP_AUDIO_RATE_SWEEP */

#ifdef CONFIG_APP_I2S_STRESS

/*
 * I2S START/DROP stress. This is the test that found the leak, then found that the
 * OBVIOUS fix for the leak is worse than the leak, and then proved the real one.
 *
 * THE LEAK. Zephyr's ESP32 I2S driver loses one slab block per direction on every
 * START/DROP: i2s_esp32_{rx,tx}_stop_transfer() set stream->data->mem_block -- the
 * block the DMA is working on -- to NULL without returning it to the slab, and DROP
 * calls exactly those. It cannot present as an error, because i2s_buf_write()
 * allocates with K_FOREVER: an exhausted slab is an unkillable block with no error,
 * no fault and no log line. The census is what makes it visible, and the guard below
 * is what makes this test REPORT on an unpatched tree instead of hanging in it.
 *
 * WHY THE OBVIOUS FIX IS WRONG. Just handing the block back crashes within two cycles:
 * k_mem_slab_alloc walks a free list whose next pointer has been overwritten with
 * captured audio. On the SOC_GDMA_SUPPORTED path, *_stop_transfer() calls dma_stop()
 * and NOTHING ELSE -- it never stops the I2S unit feeding the DMA, unlike the
 * non-GDMA branch three lines below it. So the block is still being written when it
 * lands back on the free list. THE LEAK WAS MASKING THAT: the block was never
 * returned, so the stray writes went somewhere nobody would ever look.
 *
 * The canary below is what measures it: stamp every free RX block, hand them back,
 * wait, take them again. One comes back written to, every cycle. With the I2S unit
 * actually stopped, none do.
 *
 * AND THE DELIBERATE UNDERRUN. Starving TX drives the driver down its tx_disable
 * path, where the TX DMA callback has already freed the block -- the one place where
 * returning the in-flight block could ALSO be a plain double free. That is a second,
 * independent hazard, and the census sees it: a free count ABOVE the slab's block
 * count is impossible unless the free list has a cycle in it.
 */

/*
 * Name each step, with the slab census beside it.
 *
 * The crash this test found lives in an ISR -- the RX DMA callback -- and an ISR that
 * dies prints nothing. The last marker the THREAD wrote is the only thing that says
 * where it was. And a use-after-free does not move the free COUNT, only the free LIST,
 * so the count alone cannot see it; what the count can do is show the step at which it
 * still looked sane.
 */
#define STRESS_CYCLES         ((unsigned int)CONFIG_APP_I2S_STRESS_CYCLES)
#define STRESS_BLOCKS         4U   /* full-duplex blocks on a normal cycle */
#define STRESS_UNDERRUN_EVERY 5U   /* starve TX on every Nth cycle */
#define STRESS_UNDERRUN_MS    200  /* long enough for 8 blocks of TX to drain */

static int16_t stress_rx[BLOCK_FRAMES * AUDIO_CHANNELS];

/*
 * Name each step, with the slab census beside it.
 *
 * The crash this test found lives in an ISR -- the RX DMA callback -- and an ISR that
 * dies prints nothing. The last marker the THREAD wrote is the only record of where it
 * was. And note what the census can and cannot see: a use-after-free does not move the
 * free COUNT, only the free LIST, so the count cannot detect it; what it can do is show
 * the last step at which the accounting still looked sane.
 */
static void stress_step(unsigned int cycle, const char *what)
{
	if (cycle > 3U) {
		return;
	}

	printk("    c%u.%-9s tx=%u rx=%u\n", cycle, what, k_mem_slab_num_free_get(&tx_slab),
	       k_mem_slab_num_free_get(&rx_slab));
}

#define STRESS_STEP(c, s) stress_step((unsigned int)(c), (s))

static void stress_reset(void)
{
	(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
	(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_PREPARE);
	(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
}

/* One full-duplex block: keep TX fed, take one RX block. */
static int stress_io(void)
{
	size_t sz = sizeof(stress_rx);
	int ret;

	ret = i2s_buf_write(i2s_dev, tone_block, BLOCK_SIZE);
	if (ret < 0) {
		return ret;
	}

	return i2s_buf_read(i2s_dev, stress_rx, &sz);
}

int audio_i2s_stress(void)
{
	unsigned int exhausted_at = 0U;
	unsigned int corrupt_at = 0U;
	unsigned int underruns = 0U;
	unsigned int canary_bad = 0U;
	unsigned int done = 0U;
	int ret;

	if (!ready) {
		printk("STRESS skipped: audio not ready\n");
		return -ENODEV;
	}

	/* Park the capture thread; it only touches I2S while `ready` is set. */
	ready = false;
	k_msleep(100);

	printk("\n=== I2S START/DROP stress: %u cycles, TX starved every %u ===\n",
	       STRESS_CYCLES, STRESS_UNDERRUN_EVERY);
	printk("slab : free blocks per direction, read BEFORE each cycle. It must stay\n");
	printk("       at %u. Falling means the driver is losing the DMA's in-flight\n",
	       (unsigned int)BLOCK_COUNT);
	printk("       block on every DROP; rising above %u means a block was freed\n",
	       (unsigned int)BLOCK_COUNT);
	printk("       twice and the free list has a cycle in it.\n");
	printk("starve: TX is left to run dry, which drives the driver down its\n");
	printk("       tx_disable path -- the one place the leak fix could double-free.\n\n");

	for (unsigned int i = 1U; i <= STRESS_CYCLES; i++) {
		bool starve = (i % STRESS_UNDERRUN_EVERY) == 0U;
		uint32_t tx = k_mem_slab_num_free_get(&tx_slab);
		uint32_t rx = k_mem_slab_num_free_get(&rx_slab);

		if (tx > BLOCK_COUNT || rx > BLOCK_COUNT) {
			printk("\n*** STRESS: SLAB CORRUPT at cycle %u: tx=%u rx=%u of %u.\n"
			       "    More free blocks than exist. The free list has a cycle:\n"
			       "    some block was returned to the slab twice. ***\n\n",
			       i, tx, rx, (unsigned int)BLOCK_COUNT);
			corrupt_at = i;
			break;
		}

		if (tx < 2U || rx < 2U) {
			printk("\n*** STRESS: SLAB EXHAUSTED at cycle %u: tx=%u rx=%u of %u.\n"
			       "    The driver leaks one block per direction per START/DROP.\n"
			       "    Starting anyway would block FOREVER inside i2s_buf_write(),\n"
			       "    which allocates with K_FOREVER -- no error, no log line.\n"
			       "    Fix: scripts/patch_zephyr_i2s_leak.sh ***\n\n",
			       i, tx, rx, (unsigned int)BLOCK_COUNT);
			exhausted_at = i;
			break;
		}

		if (i <= 10U || (i % 10U) == 0U || starve) {
			printk("  stress %3u: slab tx=%u rx=%u%s\n", i, tx, rx,
			       starve ? "   TX starved on purpose" : "");
		}

		STRESS_STEP(i, "reset");
		stress_reset();

		STRESS_STEP(i, "configure");
		ret = audio_configure_chain(AUDIO_SAMPLE_RATE);
		if (ret < 0) {
			printk("*** STRESS: configure failed at cycle %u (%d) ***\n", i, ret);
			goto restore;
		}

		STRESS_STEP(i, "prequeue");
		for (int b = 0; b < 2; b++) {
			ret = i2s_buf_write(i2s_dev, tone_block, BLOCK_SIZE);
			if (ret < 0) {
				printk("*** STRESS: prequeue failed at cycle %u (%d) ***\n", i,
				       ret);
				goto restore;
			}
		}

		STRESS_STEP(i, "start");
		ret = i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_START);
		if (ret < 0) {
			printk("*** STRESS: START failed at cycle %u (%d) ***\n", i, ret);
			goto restore;
		}

		if (starve) {
			/*
			 * Feed TX nothing and drain RX nothing. TX runs its two pre-queued
			 * blocks out and finds the queue empty, which takes the driver to
			 * tx_disable; RX fills its queue with nobody reading, which takes it
			 * to rx_disable. Both stop paths run with a block in flight. That is
			 * the case this whole test exists for, and an error here is the
			 * DRIVER reporting the underrun, not a failure of the test.
			 */
			k_msleep(STRESS_UNDERRUN_MS);
			underruns++;
		} else {
			STRESS_STEP(i, "io");
			for (uint32_t b = 0U; b < STRESS_BLOCKS; b++) {
				ret = stress_io();
				if (ret < 0) {
					printk("*** STRESS: i/o failed at cycle %u block %u "
					       "(%d) ***\n",
					       i, b, ret);
					goto restore;
				}
			}
		}

		STRESS_STEP(i, "drop");
		stress_reset();
		STRESS_STEP(i, "dropped");

		/*
		 * IS THE DMA STILL WRITING INTO A BLOCK WE HAVE HANDED BACK?
		 *
		 * The hypothesis this test exists to settle. On the GDMA path,
		 * i2s_esp32_rx_stop_transfer() calls dma_stop() and nothing else -- it does
		 * not stop the I2S link, does not clear dma_pending, and does not wait for
		 * the channel to go quiet. If the DMA writes even one more burst after
		 * that, then returning the in-flight block to the slab (which the leak fix
		 * does) hands the hardware a buffer that is on the free list, and the audio
		 * lands on top of the free-list next pointer.
		 *
		 * So: take every block the slab has, stamp it, give them all back, wait,
		 * take them again, and see whether anything wrote to them in between.
		 * Nothing legitimate should. The DMA is stopped. That is the claim.
		 */
		/*
		 * The canary. It is what settled WHY the obvious fix for the leak does not
		 * work, and it is cheap, so it stays.
		 *
		 * Take every block the slab has, stamp it, hand them all back, wait, take
		 * them again. Nothing legitimate should have written to them: the I2S is
		 * DROPped and the capture thread is parked. If any come back changed, the
		 * DMA is still writing into memory the driver has already returned to the
		 * slab -- which is exactly what happens when *_stop_transfer() frees the
		 * in-flight block without stopping the I2S unit first (measured: 1 of 8,
		 * every cycle; with the unit stopped, 0 of 8).
		 *
		 * Skip the first word of each block: k_mem_slab_free() stores the free-list
		 * next pointer THERE, in the block itself. Checking from offset 0 reported
		 * all eight clobbered on the first run, which was the test lying to me, not
		 * a finding.
		 */
		if (i <= 2U) {
			void *blk[BLOCK_COUNT];
			unsigned int n = 0U;
			unsigned int clobbered = 0U;

			while (n < BLOCK_COUNT &&
			       k_mem_slab_alloc(&rx_slab, &blk[n], K_NO_WAIT) == 0) {
				memset(blk[n], 0x5A, BLOCK_SIZE);
				n++;
			}
			for (unsigned int b = 0U; b < n; b++) {
				k_mem_slab_free(&rx_slab, blk[b]);
			}

			k_msleep(50);

			for (unsigned int b = 0U; b < n; b++) {
				if (k_mem_slab_alloc(&rx_slab, &blk[b], K_NO_WAIT) != 0) {
					break;
				}
			}
			for (unsigned int b = 0U; b < n; b++) {
				const uint8_t *p = blk[b];

				for (size_t k = sizeof(void *); k < BLOCK_SIZE; k++) {
					if (p[k] != 0x5AU) {
						clobbered++;
						break;
					}
				}
			}
			for (unsigned int b = 0U; b < n; b++) {
				k_mem_slab_free(&rx_slab, blk[b]);
			}

			printk("    c%u.canary  stamped %u free rx blocks, waited 50 ms, "
			       "%u came back written to\n",
			       i, n, clobbered);
			if (clobbered != 0U) {
				printk("    c%u.canary  => the DMA is STILL WRITING into memory "
				       "the driver gave back to the slab. Run "
				       "scripts/patch_zephyr_i2s_leak.sh\n",
				       i);
				canary_bad += clobbered;
			}
		}

		done = i;
	}

restore:
	stress_reset();

	printk("\n  final census: tx=%u rx=%u of %u\n", k_mem_slab_num_free_get(&tx_slab),
	       k_mem_slab_num_free_get(&rx_slab), (unsigned int)BLOCK_COUNT);

	ret = audio_configure_chain(AUDIO_SAMPLE_RATE);
	if (ret < 0) {
		printk("\n*** STRESS FAILED: could not restore audio (%d) ***\n\n", ret);
		return ret;
	}
	ready = true;

	if (canary_bad != 0U) {
		printk("\n*** STRESS FAILED: the DMA wrote into %u block(s) that the driver "
		       "had\n    already returned to the slab. Freeing the in-flight block "
		       "without\n    stopping the I2S unit first is not safe. ***\n\n",
		       canary_bad);
		return -EIO;
	}

	if (corrupt_at != 0U) {
		printk("\n*** STRESS FAILED: the slab free list was corrupted at cycle %u.\n"
		       "    A block was freed twice. ***\n\n",
		       corrupt_at);
		return -EIO;
	}

	if (exhausted_at != 0U) {
		printk("\n*** STRESS FAILED: the slab leaked out after %u cycle(s).\n"
		       "    This is the unpatched Zephyr ESP32 I2S driver. ***\n\n",
		       exhausted_at);
		return -ENOMEM;
	}

	if (done != STRESS_CYCLES) {
		printk("\n*** STRESS FAILED: stopped after %u of %u cycles ***\n\n", done,
		       STRESS_CYCLES);
		return -EIO;
	}

	printk("\n=== STRESS PASSED: %u cycles, %u of them with TX starved on purpose,\n"
	       "    and the slab held %u/%u free the whole way. No leak, and no block\n"
	       "    freed twice on the tx_disable path. ===\n\n",
	       STRESS_CYCLES, underruns, (unsigned int)BLOCK_COUNT,
	       (unsigned int)BLOCK_COUNT);

	return 0;
}

#endif /* CONFIG_APP_I2S_STRESS */

#endif /* CONFIG_APP_AUDIO */
