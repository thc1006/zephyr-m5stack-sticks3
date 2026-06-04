/*
 * Copyright (c) 2026 Hsiu-Chi Tsai
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>

#include "audio_dsp.h"

ZTEST(audio_dsp, test_isqrt)
{
	zassert_equal(audio_isqrt(0), 0U, "isqrt(0)");
	zassert_equal(audio_isqrt(1), 1U, "isqrt(1)");
	zassert_equal(audio_isqrt(2), 1U, "isqrt(2) floors");
	zassert_equal(audio_isqrt(4), 2U, "isqrt(4)");
	zassert_equal(audio_isqrt(9), 3U, "isqrt(9)");
	zassert_equal(audio_isqrt(10), 3U, "isqrt(10) floors");
	/* Both sides of two perfect-square boundaries (catches off-by-one floors). */
	zassert_equal(audio_isqrt(8), 2U, "isqrt(8) just below 9");
	zassert_equal(audio_isqrt(15), 3U, "isqrt(15) just below 16");
	zassert_equal(audio_isqrt(16), 4U, "isqrt(16)");
	zassert_equal(audio_isqrt(24), 4U, "isqrt(24) just below 25");
	zassert_equal(audio_isqrt(25), 5U, "isqrt(25)");
	zassert_equal(audio_isqrt(1073741824U), 32768U, "isqrt(32768^2)");
	zassert_equal(audio_isqrt(0xFFFFFFFFU), 65535U, "isqrt(UINT32_MAX) floors");
}

ZTEST(audio_dsp, test_rms)
{
	int16_t silence[8] = {0};
	int16_t dc[8] = {32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767};
	int16_t sq[8] = {16000, -16000, 16000, -16000, 16000, -16000, 16000, -16000};
	int16_t mixed[2] = {3000, -4000};
	int16_t one[1] = {-12345};

	zassert_equal(audio_rms_i16(silence, 8), 0U, "rms(silence) = 0");
	zassert_equal(audio_rms_i16(dc, 8), 32767U, "rms(DC full-scale) = 32767");
	zassert_equal(audio_rms_i16(sq, 8), 16000U, "rms(square +/-16000) = 16000");
	/* Mixed unequal +/- magnitudes: sqrt((3000^2 + 4000^2)/2) = sqrt(12500000). */
	zassert_equal(audio_rms_i16(mixed, 2), 3535U, "rms(3000,-4000) = 3535");
	/* n=1 (division-by-one path): RMS of one sample is its magnitude. */
	zassert_equal(audio_rms_i16(one, 1), 12345U, "rms(single sample) = |sample|");
}

ZTEST(audio_dsp, test_rms_min_and_edges)
{
	int16_t mn[4] = {INT16_MIN, INT16_MIN, INT16_MIN, INT16_MIN};

	/*
	 * RMS of a full block of -32768 = 32768. The block sum of squares is
	 * 4 * 32768^2 = 2^32, which overflows uint32; this asserts the 64-bit
	 * accumulation in audio_rms_i16.
	 */
	zassert_equal(audio_rms_i16(mn, 4), 32768U, "rms(all INT16_MIN) = 32768");
	zassert_equal(audio_rms_i16(NULL, 4), 0U, "rms(NULL) = 0");
	zassert_equal(audio_rms_i16(mn, 0), 0U, "rms(n=0) = 0");
}

ZTEST(audio_dsp, test_peak)
{
	int16_t a[4] = {0, 100, -200, 50};
	int16_t b[3] = {10, INT16_MIN, -5};
	int16_t z[3] = {0, 0, 0};
	int16_t neg[3] = {-5, -200, -50};

	zassert_equal(audio_peak_i16(a, 4), 200U, "peak abs = 200");
	zassert_equal(audio_peak_i16(b, 3), 32767U, "peak INT16_MIN clamps to 32767");
	zassert_equal(audio_peak_i16(neg, 3), 200U, "peak(all-negative) = 200");
	zassert_equal(audio_peak_i16(z, 3), 0U, "peak(silence) = 0");
	zassert_equal(audio_peak_i16(NULL, 3), 0U, "peak(NULL) = 0");
}

ZTEST(audio_dsp, test_deinterleave)
{
	const int16_t in[6] = {1, 2, 3, 4, 5, 6};
	int16_t out[3];

	out[0] = out[1] = out[2] = -1;
	audio_deinterleave(in, 3, 0, out);
	zassert_equal(out[0], 1, "ch0[0]");
	zassert_equal(out[1], 3, "ch0[1]");
	zassert_equal(out[2], 5, "ch0[2]");

	audio_deinterleave(in, 3, 1, out);
	zassert_equal(out[0], 2, "ch1[0]");
	zassert_equal(out[1], 4, "ch1[1]");
	zassert_equal(out[2], 6, "ch1[2]");

	/* slot > 1 (e.g. a stray channel COUNT of 2) is a no-op, NOT slot 1. */
	out[0] = 42;
	audio_deinterleave(in, 3, 2, out);
	zassert_equal(out[0], 42, "slot>1 is a no-op (count arg does not alias slot 1)");

	/* NULL input is a no-op (out left unchanged). */
	out[0] = 7;
	audio_deinterleave(NULL, 3, 0, out);
	zassert_equal(out[0], 7, "NULL in is a no-op");

	/*
	 * frames=1 boundary, plus a realistic silent-slot block (mono mic on slot 0,
	 * slot 1 silent). Extracting the wrong slot yields all-zero - the silent
	 * failure HW-016 must not mask - so pin both slots.
	 */
	{
		const int16_t silent_slot[4] = {1000, 0, 2000, 0};
		int16_t two[2];

		audio_deinterleave(silent_slot, 1, 0, two);
		zassert_equal(two[0], 1000, "frames=1 slot0");

		audio_deinterleave(silent_slot, 2, 0, two);
		zassert_equal(two[0], 1000, "slot0[0]");
		zassert_equal(two[1], 2000, "slot0[1]");

		two[0] = two[1] = -1;
		audio_deinterleave(silent_slot, 2, 1, two);
		zassert_equal(two[0], 0, "slot1[0] silent");
		zassert_equal(two[1], 0, "slot1[1] silent");
	}
}

ZTEST(audio_dsp, test_level_bars)
{
	zassert_equal(audio_level_bars(0, 100, 4), 0U, "0 -> 0 bars");
	zassert_equal(audio_level_bars(100, 100, 4), 4U, "full -> nbars");
	zassert_equal(audio_level_bars(50, 100, 4), 2U, "half -> nbars/2");
	zassert_equal(audio_level_bars(25, 100, 4), 1U, "quarter -> 1");
	zassert_equal(audio_level_bars(150, 100, 4), 4U, "over-full clamps to nbars");
	zassert_equal(audio_level_bars(50, 0, 4), 0U, "full=0 -> 0");
	zassert_equal(audio_level_bars(50, 100, 0), 0U, "nbars=0 -> 0");
}

ZTEST_SUITE(audio_dsp, NULL, NULL, NULL, NULL, NULL);
