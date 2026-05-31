/* SPDX-License-Identifier: Apache-2.0 */
#include "nec.h"

#include <stdbool.h>

/* Inclusive range check with explicit tolerance windows (NEC is ~+/-20%). */
static inline bool in_range(uint32_t v, uint32_t lo, uint32_t hi)
{
	return (v >= lo) && (v <= hi);
}

uint32_t nec_encode(uint8_t addr, uint8_t cmd)
{
	return (uint32_t)addr |
	       ((uint32_t)(uint8_t)~addr << 8) |
	       ((uint32_t)cmd << 16) |
	       ((uint32_t)(uint8_t)~cmd << 24);
}

enum nec_decode_status nec_decode(const struct nec_event *ev, size_t n,
				  struct nec_frame *out)
{
	uint32_t frame = 0;
	uint8_t addr;
	uint8_t addr_inv;
	uint8_t cmd;
	uint8_t cmd_inv;

	if (n < 1U) {
		return NEC_DECODE_NO_LEADER;
	}

	/* Leader mark ~9 ms (accept a wide tolerance). */
	if (!in_range(ev[0].mark_us, 7000U, 11000U)) {
		return NEC_DECODE_NO_LEADER;
	}

	/* Repeat frame: 9 ms mark + ~2.25 ms space. */
	if (in_range(ev[0].space_us, 1800U, 2700U)) {
		return NEC_DECODE_REPEAT;
	}

	/* Full-frame leader: 9 ms mark + ~4.5 ms space. */
	if (!in_range(ev[0].space_us, 3500U, 5500U)) {
		return NEC_DECODE_NO_LEADER;
	}

	if (n < NEC_FRAME_EVENTS) {
		return NEC_DECODE_TRUNCATED;
	}

	for (uint32_t i = 0; i < 32U; i++) {
		const struct nec_event *b = &ev[1U + i];

		/* Every bit starts with a ~560 us mark. */
		if (!in_range(b->mark_us, 200U, 900U)) {
			return NEC_DECODE_BAD_BIT;
		}

		/* 0 -> ~560 us space, 1 -> ~1690 us space. */
		if (in_range(b->space_us, 200U, 900U)) {
			/* bit 0: leave the bit cleared */
		} else if (in_range(b->space_us, 1200U, 2200U)) {
			frame |= (uint32_t)1U << i;
		} else {
			return NEC_DECODE_BAD_BIT;
		}
	}

	addr = (uint8_t)(frame & 0xFFU);
	addr_inv = (uint8_t)((frame >> 8) & 0xFFU);
	cmd = (uint8_t)((frame >> 16) & 0xFFU);
	cmd_inv = (uint8_t)((frame >> 24) & 0xFFU);

	if ((uint8_t)(addr ^ addr_inv) != 0xFFU ||
	    (uint8_t)(cmd ^ cmd_inv) != 0xFFU) {
		return NEC_DECODE_BAD_CHECKSUM;
	}

	if (out != NULL) {
		out->addr = addr;
		out->cmd = cmd;
	}

	return NEC_DECODE_OK;
}
