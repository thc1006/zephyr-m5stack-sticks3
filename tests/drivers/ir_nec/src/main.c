/* SPDX-License-Identifier: Apache-2.0 */
#include <stdbool.h>

#include <zephyr/ztest.h>

#include "nec.h"

/*
 * Build the 33-event (mark, space) sequence for an NEC frame carrying addr/cmd.
 * A signed jitter (us) is applied to every value to model real capture timing:
 * spaces that should be long ('1') get -jitter and everything else +jitter, so
 * the windows are exercised from both sides without leaving tolerance.
 */
static size_t build_frame(struct nec_event *ev, uint8_t addr, uint8_t cmd, int jitter)
{
	uint32_t frame = nec_encode(addr, cmd);

	ev[0].mark_us = (uint16_t)((int)NEC_LEADER_MARK_US + jitter);
	ev[0].space_us = (uint16_t)((int)NEC_LEADER_SPACE_US - jitter);

	for (uint32_t i = 0; i < 32U; i++) {
		bool one = ((frame >> i) & 1U) != 0U;

		ev[1U + i].mark_us = (uint16_t)((int)NEC_BIT_MARK_US + jitter);
		ev[1U + i].space_us =
			(uint16_t)((int)(one ? NEC_ONE_SPACE_US : NEC_ZERO_SPACE_US) +
				   (one ? -jitter : jitter));
	}

	return NEC_FRAME_EVENTS;
}

ZTEST_SUITE(ir_nec, NULL, NULL, NULL, NULL, NULL);

ZTEST(ir_nec, test_encode_checksum)
{
	uint32_t f = nec_encode(0x00, 0xAD);

	zassert_equal(f & 0xFFU, 0x00U, "addr byte");
	zassert_equal((f >> 8) & 0xFFU, 0xFFU, "~addr byte");
	zassert_equal((f >> 16) & 0xFFU, 0xADU, "cmd byte");
	zassert_equal((f >> 24) & 0xFFU, 0x52U, "~cmd byte");
}

ZTEST(ir_nec, test_decode_ideal)
{
	struct nec_event ev[NEC_FRAME_EVENTS];
	struct nec_frame out;
	size_t n = build_frame(ev, 0x04, 0x1B, 0);

	zassert_equal(nec_decode(ev, n, &out), NEC_DECODE_OK, "ideal frame");
	zassert_equal(out.addr, 0x04, "addr");
	zassert_equal(out.cmd, 0x1B, "cmd");
}

ZTEST(ir_nec, test_decode_jittered)
{
	struct nec_event ev[NEC_FRAME_EVENTS];
	struct nec_frame out;
	/* +/-80 us jitter on the 560 us base unit is well within NEC tolerance. */
	size_t n = build_frame(ev, 0x12, 0x34, 80);

	zassert_equal(nec_decode(ev, n, &out), NEC_DECODE_OK, "jittered frame");
	zassert_equal(out.addr, 0x12, "addr");
	zassert_equal(out.cmd, 0x34, "cmd");
}

ZTEST(ir_nec, test_roundtrip_all_commands)
{
	/* Every command 0..255 must encode then decode back unchanged. */
	for (unsigned int c = 0; c <= 0xFFU; c++) {
		struct nec_event ev[NEC_FRAME_EVENTS];
		struct nec_frame out;
		size_t n = build_frame(ev, 0xA1, (uint8_t)c, 0);

		zassert_equal(nec_decode(ev, n, &out), NEC_DECODE_OK, "cmd %u", c);
		zassert_equal(out.cmd, (uint8_t)c, "cmd %u roundtrip", c);
		zassert_equal(out.addr, 0xA1, "addr for cmd %u", c);
	}
}

ZTEST(ir_nec, test_decode_repeat)
{
	struct nec_event ev[1];

	ev[0].mark_us = NEC_LEADER_MARK_US;
	ev[0].space_us = NEC_REPEAT_SPACE_US;

	zassert_equal(nec_decode(ev, 1, NULL), NEC_DECODE_REPEAT, "repeat frame");
}

ZTEST(ir_nec, test_decode_no_leader)
{
	struct nec_event ev[NEC_FRAME_EVENTS];
	struct nec_frame out;

	(void)build_frame(ev, 0x04, 0x1B, 0);
	ev[0].mark_us = 2000; /* too short to be a 9 ms leader */

	zassert_equal(nec_decode(ev, NEC_FRAME_EVENTS, &out), NEC_DECODE_NO_LEADER,
		      "bad leader rejected");
}

ZTEST(ir_nec, test_decode_truncated)
{
	struct nec_event ev[NEC_FRAME_EVENTS];
	struct nec_frame out;

	(void)build_frame(ev, 0x04, 0x1B, 0);

	zassert_equal(nec_decode(ev, 10, &out), NEC_DECODE_TRUNCATED,
		      "short event count rejected");
}

ZTEST(ir_nec, test_decode_bad_checksum)
{
	struct nec_event ev[NEC_FRAME_EVENTS];
	struct nec_frame out;

	(void)build_frame(ev, 0x04, 0x1B, 0);
	/* Flip cmd bit 0 (event index 1 + 16) so cmd no longer matches ~cmd. */
	ev[1U + 16U].space_us = (ev[1U + 16U].space_us > 1000U) ? NEC_ZERO_SPACE_US
							       : NEC_ONE_SPACE_US;

	zassert_equal(nec_decode(ev, NEC_FRAME_EVENTS, &out), NEC_DECODE_BAD_CHECKSUM,
		      "checksum mismatch rejected");
}

ZTEST(ir_nec, test_decode_bad_bit)
{
	struct nec_event ev[NEC_FRAME_EVENTS];
	struct nec_frame out;

	(void)build_frame(ev, 0x04, 0x1B, 0);
	ev[5].space_us = 5000; /* neither a valid 0 nor 1 space */

	zassert_equal(nec_decode(ev, NEC_FRAME_EVENTS, &out), NEC_DECODE_BAD_BIT,
		      "out-of-window bit rejected");
}
