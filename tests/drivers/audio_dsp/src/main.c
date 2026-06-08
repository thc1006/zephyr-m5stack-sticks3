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

ZTEST(audio_dsp, test_interleave_mono)
{
	const int16_t mono[3] = {1, -2, 3};
	int16_t out[6];

	for (int i = 0; i < 6; i++) {
		out[i] = -1;
	}
	audio_interleave_mono(mono, 3, out);
	zassert_equal(out[0], 1, "L[0]");
	zassert_equal(out[1], 1, "R[0]");
	zassert_equal(out[2], -2, "L[1]");
	zassert_equal(out[3], -2, "R[1]");
	zassert_equal(out[4], 3, "L[2]");
	zassert_equal(out[5], 3, "R[2]");

	/* Round-trip: mono -> stereo -> either slot == mono (inverse of deinterleave). */
	{
		int16_t back0[3];
		int16_t back1[3];

		audio_deinterleave(out, 3, 0, back0);
		audio_deinterleave(out, 3, 1, back1);
		zassert_mem_equal(back0, mono, sizeof(mono), "slot0 round-trips mono");
		zassert_mem_equal(back1, mono, sizeof(mono), "slot1 round-trips mono");
	}

	/* frames=1 boundary. */
	out[0] = out[1] = -1;
	audio_interleave_mono(mono, 1, out);
	zassert_equal(out[0], 1, "frames=1 L");
	zassert_equal(out[1], 1, "frames=1 R");

	/* NULL / frames=0 are no-ops (out left unchanged). */
	out[0] = 7;
	audio_interleave_mono(NULL, 3, out);
	zassert_equal(out[0], 7, "NULL mono is a no-op");
	out[0] = 9;
	audio_interleave_mono(mono, 0, out);
	zassert_equal(out[0], 9, "frames=0 is a no-op");
}

ZTEST(audio_dsp, test_gain_clip)
{
	const int16_t in[4] = {100, -100, 10000, -10000};
	int16_t out[4];

	/* Unity gain (256) is the identity. */
	audio_gain_clip_i16(in, 4, 256, out);
	zassert_mem_equal(out, in, sizeof(in), "gain x1.0 = identity");

	/* 2.0x doubles within range. */
	audio_gain_clip_i16(in, 2, 512, out);
	zassert_equal(out[0], 200, "x2.0 of 100");
	zassert_equal(out[1], -200, "x2.0 of -100");

	/* 1.5x is fractional (Q8): 100*384/256 = 150. */
	audio_gain_clip_i16(in, 1, 384, out);
	zassert_equal(out[0], 150, "x1.5 of 100");

	/* Saturation: 10000*4 = 40000 -> +32767; -10000*4 = -40000 -> -32768. */
	{
		const int16_t loud[2] = {10000, -10000};
		int16_t o[2];

		audio_gain_clip_i16(loud, 2, 1024, o);
		zassert_equal(o[0], 32767, "positive clips to +32767");
		zassert_equal(o[1], -32768, "negative clips to -32768");
	}

	/* INT16_MIN at unity stays INT16_MIN (no overflow in the int32 product). */
	{
		const int16_t mn[1] = {INT16_MIN};
		int16_t o[1];

		audio_gain_clip_i16(mn, 1, 256, o);
		zassert_equal(o[0], INT16_MIN, "INT16_MIN x1.0 unchanged");
	}

	/* gain 0 -> silence. */
	audio_gain_clip_i16(in, 4, 0, out);
	zassert_equal(out[0], 0, "gain 0 -> 0");
	zassert_equal(out[2], 0, "gain 0 -> 0 (loud sample)");

	/* In-place (out == in) is safe (read before write at the same index). */
	{
		int16_t buf[3] = {1000, -2000, 3000};

		audio_gain_clip_i16(buf, 3, 512, buf);
		zassert_equal(buf[0], 2000, "in-place x2.0 [0]");
		zassert_equal(buf[1], -4000, "in-place x2.0 [1]");
		zassert_equal(buf[2], 6000, "in-place x2.0 [2]");
	}

	/* NULL / n=0 are no-ops (out left unchanged). */
	out[0] = 5;
	audio_gain_clip_i16(NULL, 4, 256, out);
	zassert_equal(out[0], 5, "NULL in is a no-op");
	out[0] = 6;
	audio_gain_clip_i16(in, 0, 256, out);
	zassert_equal(out[0], 6, "n=0 is a no-op");
}

ZTEST_SUITE(audio_dsp, NULL, NULL, NULL, NULL, NULL);
