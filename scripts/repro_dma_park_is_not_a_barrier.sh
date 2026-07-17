#!/usr/bin/env bash
# Copyright (c) 2026 Hsiu-Chi Tsai
# SPDX-License-Identifier: Apache-2.0
#
# A MEASUREMENT TOOL, NOT A FIX. Do not apply this to a tree that produces a patch for
# review, and do not cite anything it prints as evidence of memory safety.
#
# It exists to settle one question with a number: does waiting for the GDMA descriptor
# FSM to park make it safe to hand the DMA's buffer back to a mem-slab?
#
# It does not. Both halves of that were measured on an ESP32-S3:
#
#   * With the wait in place, 874 dma_stop() calls returned with the FSM ALREADY IDLE.
#     Not one of them spun. The wait is free, and it is free because it never waits.
#   * With the I2S RX unit left running, the DMA went on writing into the freed block
#     regardless -- canary caught 1 of 8 blocks overwritten every cycle, and the run died
#     at cycle 5 with EXCCAUSE 28 inside k_mem_slab_alloc().
#   * Adding a channel reset (gdma_ll_rx_reset_channel: "Reset DMA RX channel FSM and
#     FIFO pointer") on top did not help either. Same canary, same crash.
#
# So INLINK_PARK observes the DESCRIPTOR FSM -- "nothing left to fetch" -- and says
# nothing about bytes still in flight. It is the wrong observable, and a wait on it is a
# fix that fixes nothing while looking like a proof. That is why this is a separate
# script: so that it can never be mistaken for part of the real one.
#
# What actually stops the writes is stopping the PRODUCER: i2s_hal_rx_stop() before
# dma_stop(), which is also what ESP-IDF's own I2S driver does. See
# scripts/patch_zephyr_i2s_leak.sh -- the fix.
#
# Everything below is the original rationale, kept because the contract complaint it
# describes is still true; it just cannot be repaired from inside the DMA driver.
#
# ----------------------------------------------------------------------------------
#
# Make dma_stop() on ESP32 GDMA mean what the Zephyr DMA API says it means.
#
# THE CONTRACT. include/zephyr/drivers/dma.h defines dma_stop() as "Stops the DMA
# transfer and disables the channel." A caller that gets 0 back is entitled to reuse,
# free, or hand back the buffer it was transferring. Every DMA client in the tree
# assumes exactly that.
#
# WHAT THE ESP32 GDMA DRIVER ACTUALLY DOES (drivers/dma/dma_esp32_gdma.c):
#
#     static int dma_esp32_stop(const struct device *dev, uint32_t channel)
#     {
#             ...
#             gdma_ll_rx_enable_interrupt(..., false);
#             gdma_ll_rx_stop(data->hal.dev, dma_channel->channel_id);
#             ...
#             return 0;
#     }
#
# It masks the interrupt, writes the STOP bit, and returns. It never observes whether
# the transfer actually stopped.
#
# STOP IS A COMMAND. PARK IS THE OBSERVATION. They are different bits, and the hardware
# is explicit about it. The ESP32-S3 TRM defines GDMA_INLINK_STOP_CHn / GDMA_OUTLINK_STOP_CHn
# ("stop the channel") and GDMA_INLINK_PARK_CHn / GDMA_OUTLINK_PARK_CHn ("the descriptor
# FSM is idle") as separate fields, and the HAL exposes them separately too:
#
#     gdma_ll_rx_stop()            -> writes in.link.stop = 1
#     gdma_ll_rx_is_desc_fsm_idle()-> reads  in.link.park
#
# Nothing in the TRM says that when the CPU's MMIO write to STOP retires, every AHB
# transaction the channel had already issued has landed.
#
# AND THIS DRIVER ALREADY KNOWS THAT. Sixty lines below dma_esp32_stop(), its own
# get_status() computes busy from exactly the bit that stop() declines to look at:
#
#     status->busy = !gdma_ll_rx_is_desc_fsm_idle(data->hal.dev, dma_channel->channel_id);
#
# WHY IT MATTERS. It is not theoretical. drivers/i2s/i2s_esp32.c hands the DMA's block
# straight back to a k_mem_slab after dma_stop() returns, and the hardware wrote captured
# audio over the slab's free-list next pointer:
#
#     FATAL EXCEPTION: EXCCAUSE 28 (load prohibited)
#     PC   k_mem_slab_alloc      (kernel/mem_slab.c)
#     VADDR 0x220022   <- 0x0022, 0x0022: two int16 audio samples, not a pointer
#
# Any client that frees or reuses a buffer on the strength of a successful dma_stop() has
# the same exposure. Fixing it in the client is a workaround; the contract belongs here.
#
# THE FIX. Poll the descriptor FSM's park bit, bounded, and fail rather than lie:
# dma_stop() returns 0 only when the channel really is finished with memory.
#
# AND PARK IS NOT ENOUGH. Measured on an ESP32-S3, and this is the part that matters:
#
#   With the park wait in place, 874 dma_stop() calls returned with the descriptor FSM
#   ALREADY idle -- not one of them ever spun. And with the I2S RX unit left running, the
#   DMA went on writing into the freed block anyway: the canary caught 1 of 8 blocks
#   overwritten on every cycle, and the run died at cycle 5 with EXCCAUSE 28.
#
# So the descriptor FSM parks while the data path is still draining. PARK says "no more
# descriptors to fetch", not "no more bytes in flight". A wait on it is a real fix for the
# API contract and a useless one for memory safety.
#
# What DOES quiesce the channel is a reset. gdma_ll_rx_reset_channel() is documented as
# "Reset DMA RX channel FSM and FIFO pointer" -- FSM *and FIFO*, which is precisely the
# state the park bit cannot see. And "disables the channel" is what the Zephyr DMA API
# already promises dma_stop() does.
#
# Usage: bash scripts/patch_zephyr_dma_park.sh <zephyr-tree> [--measure] [--reset]
#
#   --measure  export counters so that "the FSM was still busy" is a MEASUREMENT and not
#              an assumption. A fix for a race nobody has observed should say so.
#   --reset    reset the channel (FSM + FIFO pointer) after the stop, instead of only
#              waiting for the descriptor FSM to park.
set -euo pipefail

Z=${1:?usage: patch_zephyr_dma_park.sh <zephyr-tree> [--measure] [--reset]}
F=$Z/drivers/dma/dma_esp32_gdma.c
MEASURE=
RESET=
for a in "${@:2}"; do
	case "$a" in
	--measure) MEASURE=1 ;;
	--reset) RESET=1 ;;
	*) echo "unknown option: $a" >&2; exit 1 ;;
	esac
done

[ -f "$F" ] || { echo "not a Zephyr tree (no $F)" >&2; exit 1; }

MEASURE=$MEASURE RESET=$RESET python3 - "$F" <<'PY'
import io
import os
import sys

path = sys.argv[1]
measure = os.environ.get("MEASURE", "") == "1"
reset = os.environ.get("RESET", "") == "1"
src = io.open(path, encoding="utf-8", newline="\n").read()

if "dma_esp32_wait_park" in src:
    print("  already patched; nothing to do")
    raise SystemExit(0)

COUNTERS = """
/*
 * Instrumentation, so that "the descriptor FSM was still busy when dma_stop() returned"
 * is a measurement and not an assertion. A fix for a race nobody has observed is a fix
 * whose value nobody can check.
 */
volatile uint32_t dma_esp32_park_spins_max;
volatile uint32_t dma_esp32_park_busy_stops;
volatile uint32_t dma_esp32_park_total_stops;
volatile uint32_t dma_esp32_park_timeouts;
""" if measure else ""

RECORD_BUSY = """
	dma_esp32_park_total_stops++;
	if (spins > 0U) {
		dma_esp32_park_busy_stops++;
		if (spins > dma_esp32_park_spins_max) {
			dma_esp32_park_spins_max = spins;
		}
	}
""" if measure else ""

RECORD_TIMEOUT = """
			dma_esp32_park_timeouts++;
""" if measure else ""

WAIT = COUNTERS + """
/*
 * gdma_ll_*_stop() writes the STOP bit. That is a REQUEST. The descriptor FSM is idle
 * only when its PARK bit reads back, which is a different register field -- and it is
 * the one this driver's own get_status() uses to answer "busy".
 *
 * A bounded spin: the FSM parks within a handful of bus cycles once the channel has
 * stopped issuing transactions, so this normally exits on the first read. It is a spin
 * rather than a sleep because dma_stop() is reachable from an ISR (the ESP32 I2S driver
 * calls it from its DMA completion callbacks).
 */
#define DMA_ESP32_PARK_SPINS 20000U

static int dma_esp32_wait_park(struct dma_esp32_data *data,
			       struct dma_esp32_channel *dma_channel)
{
	uint32_t spins = 0U;

	for (;;) {
		bool idle;

		if (dma_channel->dir == DMA_RX) {
			idle = gdma_ll_rx_is_desc_fsm_idle(data->hal.dev,
							   dma_channel->channel_id);
		} else if (dma_channel->dir == DMA_TX) {
			idle = gdma_ll_tx_is_desc_fsm_idle(data->hal.dev,
							   dma_channel->channel_id);
		} else {
			return 0;
		}

		if (idle) {
			break;
		}

		if (++spins >= DMA_ESP32_PARK_SPINS) {""" + RECORD_TIMEOUT + """
			return -ETIMEDOUT;
		}
	}
""" + RECORD_BUSY + """
	return 0;
}
"""

ANCHOR = "static int dma_esp32_stop(const struct device *dev, uint32_t channel)"
assert src.count(ANCHOR) == 1, "dma_esp32_stop anchor matched %d times" % src.count(ANCHOR)
src = src.replace(ANCHOR, WAIT.strip() + "\n\n" + ANCHOR, 1)

# dma_stop() must not report success until the channel has actually parked.
OLD = """	if (dma_channel->dir == DMA_RX) {
		gdma_ll_rx_enable_interrupt(data->hal.dev, dma_channel->channel_id,
					    GDMA_LL_RX_EVENT_MASK, false);
		gdma_ll_rx_stop(data->hal.dev, dma_channel->channel_id);
	} else if (dma_channel->dir == DMA_TX) {
		gdma_ll_tx_enable_interrupt(data->hal.dev, dma_channel->channel_id,
					    GDMA_LL_TX_EVENT_MASK, false);
		gdma_ll_tx_stop(data->hal.dev, dma_channel->channel_id);
	}

	return 0;
}"""
NEW = """	if (dma_channel->dir == DMA_RX) {
		gdma_ll_rx_enable_interrupt(data->hal.dev, dma_channel->channel_id,
					    GDMA_LL_RX_EVENT_MASK, false);
		gdma_ll_rx_stop(data->hal.dev, dma_channel->channel_id);
	} else if (dma_channel->dir == DMA_TX) {
		gdma_ll_tx_enable_interrupt(data->hal.dev, dma_channel->channel_id,
					    GDMA_LL_TX_EVENT_MASK, false);
		gdma_ll_tx_stop(data->hal.dev, dma_channel->channel_id);
	}

	/*
	 * Do not return until the channel is finished with memory. Writing the STOP bit
	 * only asks it to stop; the caller is entitled to reuse the buffer the moment this
	 * returns 0, and at least one in-tree caller does exactly that.
	 */
	return dma_esp32_wait_park(data, dma_channel);
}"""
assert src.count(OLD) == 1, "dma_esp32_stop body anchor matched %d times" % src.count(OLD)
src = src.replace(OLD, NEW, 1)

if reset:
    # The park wait alone was measured NOT to be a memory barrier: the descriptor FSM
    # parks while the data path is still draining into the buffer. Reset the channel --
    # "Reset DMA RX channel FSM and FIFO pointer" -- which is the only thing the HAL
    # offers that reaches the FIFO, and which is what "disables the channel" ought to
    # mean.
    R_OLD = "	return dma_esp32_wait_park(data, dma_channel);\n}"
    R_NEW = """	if (dma_esp32_wait_park(data, dma_channel) < 0) {
		return -ETIMEDOUT;
	}

	/*
	 * The park bit only says the descriptor FSM has stopped FETCHING. Measured on an
	 * ESP32-S3: it reads idle on every single stop while the channel is still writing
	 * bytes into the buffer. Resetting the channel clears the FSM *and the FIFO
	 * pointer* -- Espressif's own words, on an ESP32-S3 GDMA bug report: "gdma_reset
	 * won't reset registers. It only reset the FIFO and FSM." That is the state the
	 * park bit cannot see, and it is the strongest thing the HAL offers.
	 *
	 * Whether it is ENOUGH is the question this build exists to answer. It is not a
	 * fix until it has been measured.
	 */
	if (dma_channel->dir == DMA_RX) {
		gdma_ll_rx_reset_channel(data->hal.dev, dma_channel->channel_id);
	} else if (dma_channel->dir == DMA_TX) {
		gdma_ll_tx_reset_channel(data->hal.dev, dma_channel->channel_id);
	}

	return 0;
}"""
    assert src.count(R_OLD) == 1, "reset anchor"
    src = src.replace(R_OLD, R_NEW, 1)

io.open(path, "w", encoding="utf-8", newline="\n").write(src)
print("  patched drivers/dma/dma_esp32_gdma.c")
print("    dma_esp32_stop(): bounded wait for the descriptor FSM to PARK before returning")
if measure:
    print("    + instrumentation: dma_esp32_park_{spins_max,busy_stops,total_stops,timeouts}")
PY
