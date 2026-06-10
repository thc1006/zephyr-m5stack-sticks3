/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Pure capture-DSP helpers (see audio_dsp.h). No Zephyr headers, so this builds
 * and is unit-tested on native_sim (tests/drivers/audio_dsp). Mirrors the nec.c
 * (pure) / ir.c (hardware) split used elsewhere in this repo.
 */

#include "audio_dsp.h"

uint32_t audio_isqrt(uint32_t x)
{
	uint32_t bit = 1UL << 30; /* highest power of four <= UINT32_MAX */
	uint32_t res = 0;

	while (bit > x) {
		bit >>= 2;
	}
	while (bit != 0U) {
		if (x >= res + bit) {
			x -= res + bit;
			res = (res >> 1) + bit;
		} else {
			res >>= 1;
		}
		bit >>= 2;
	}

	return res;
}

uint16_t audio_rms_i16(const int16_t *samples, size_t n)
{
	uint64_t sumsq = 0;

	if (samples == NULL || n == 0U) {
		return 0;
	}

	for (size_t i = 0; i < n; i++) {
		int32_t s = samples[i]; /* |s| <= 32768, so s*s fits int32 */

		sumsq += (uint64_t)(s * s);
	}

	/* mean square <= 32768^2 fits uint32; isqrt yields 0..32768. */
	return (uint16_t)audio_isqrt((uint32_t)(sumsq / n));
}

uint16_t audio_peak_i16(const int16_t *samples, size_t n)
{
	uint16_t peak = 0;

	if (samples == NULL || n == 0U) {
		return 0;
	}

	for (size_t i = 0; i < n; i++) {
		int32_t s = samples[i];
		uint32_t mag = (s < 0) ? (uint32_t)(-s) : (uint32_t)s;

		if (mag > 32767U) {
			mag = 32767U; /* clamp |INT16_MIN| */
		}
		if (mag > peak) {
			peak = (uint16_t)mag;
		}
	}

	return peak;
}

void audio_deinterleave(const int16_t *interleaved, size_t frames, uint8_t slot, int16_t *out)
{
	/*
	 * slot is a slot INDEX (0/1), not a channel count: slot > 1 is a no-op so a
	 * stray count argument leaves out unchanged (loud empty capture) instead of
	 * silently aliasing to slot 1.
	 */
	if (interleaved == NULL || out == NULL || frames == 0U || slot > 1U) {
		return;
	}

	for (size_t i = 0; i < frames; i++) {
		out[i] = interleaved[(i * 2U) + slot];
	}
}

uint8_t audio_level_bars(uint16_t level, uint16_t full, uint8_t nbars)
{
	if (full == 0U || nbars == 0U) {
		return 0;
	}
	if (level >= full) {
		return nbars;
	}

	return (uint8_t)(((uint32_t)level * nbars) / full);
}

void audio_interleave_mono(const int16_t *mono, size_t frames, int16_t *out)
{
	if (mono == NULL || out == NULL || frames == 0U) {
		return;
	}

	for (size_t i = 0; i < frames; i++) {
		out[(i * 2U)] = mono[i];      /* left  */
		out[(i * 2U) + 1U] = mono[i]; /* right */
	}
}

void audio_gain_clip_i16(const int16_t *in, size_t n, uint16_t gain_q8, int16_t *out)
{
	if (in == NULL || out == NULL || n == 0U) {
		return;
	}

	for (size_t i = 0; i < n; i++) {
		/* |in| <= 32768 and gain_q8 <= 65535, so the product fits int32. */
		int32_t v = ((int32_t)in[i] * (int32_t)gain_q8) / 256;

		if (v > 32767) {
			v = 32767;
		} else if (v < -32768) {
			v = -32768;
		}
		out[i] = (int16_t)v;
	}
}
