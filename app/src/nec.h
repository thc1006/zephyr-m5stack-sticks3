/* SPDX-License-Identifier: Apache-2.0 */
#ifndef M5STICKS3_NEC_H
#define M5STICKS3_NEC_H

#include <stddef.h>
#include <stdint.h>

/*
 * NEC infrared protocol encode/decode as pure functions, so they can be unit-
 * tested on native_sim without hardware. The LEDC carrier gating (TX) and the
 * MCPWM capture (RX) are thin shells that call these.
 *
 * NEC frame: a 9 ms leader mark + 4.5 ms space, then 32 bits sent LSB-first
 * (address, ~address, command, ~command); each bit is a 560 us mark followed by
 * a 560 us space for 0 or a 1690 us space for 1, then a final 560 us stop mark.
 * A held key emits a repeat frame: 9 ms mark + 2.25 ms space + stop mark.
 */

/* NEC pulse timings in microseconds (used by the TX carrier gating). */
#define NEC_LEADER_MARK_US   9000U
#define NEC_LEADER_SPACE_US  4500U
#define NEC_REPEAT_SPACE_US  2250U
#define NEC_BIT_MARK_US      560U
#define NEC_ZERO_SPACE_US    560U
#define NEC_ONE_SPACE_US     1690U
#define NEC_STOP_MARK_US     560U

/* One leader event + 32 bit events = a full frame as (mark, space) pairs. */
#define NEC_FRAME_EVENTS     33U

/* A captured mark (carrier present) followed by a space (carrier absent), us. */
struct nec_event {
	uint16_t mark_us;
	uint16_t space_us;
};

struct nec_frame {
	uint8_t addr;
	uint8_t cmd;
};

enum nec_decode_status {
	NEC_DECODE_OK = 0,            /* a valid frame; *out is filled */
	NEC_DECODE_REPEAT = 1,        /* a repeat (held key) frame */
	NEC_DECODE_NO_LEADER = -1,    /* first event is not a NEC leader */
	NEC_DECODE_TRUNCATED = -2,    /* not enough events for 32 bits */
	NEC_DECODE_BAD_BIT = -3,      /* a bit mark/space outside tolerance */
	NEC_DECODE_BAD_CHECKSUM = -4, /* addr/cmd inverse bytes mismatch */
};

/*
 * Encode an NEC frame as the 32-bit wire value, transmitted LSB-first:
 * bits[7:0]=addr, [15:8]=~addr, [23:16]=cmd, [31:24]=~cmd.
 */
uint32_t nec_encode(uint8_t addr, uint8_t cmd);

/*
 * Decode a captured (mark, space) event sequence. Returns an nec_decode_status;
 * on NEC_DECODE_OK, *out receives the decoded address and command.
 */
enum nec_decode_status nec_decode(const struct nec_event *ev, size_t n,
				  struct nec_frame *out);

#endif /* M5STICKS3_NEC_H */
