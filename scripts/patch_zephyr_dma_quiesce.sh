#!/usr/bin/env bash
# Copyright (c) 2026 Hsiu-Chi Tsai
# SPDX-License-Identifier: Apache-2.0
#
# Make dma_stop() on ESP32 GDMA mean what the Zephyr DMA API says it means: the channel
# has stopped, and the caller may reuse the buffer.
#
# THE CONTRACT. include/zephyr/drivers/dma.h defines dma_stop() as "Stops the DMA transfer
# and disables the channel." A caller that gets 0 back is entitled to free or reuse the
# buffer it was transferring, and drivers/i2s/i2s_esp32.c does exactly that.
#
# WHAT IT ACTUALLY DID (drivers/dma/dma_esp32_gdma.c):
#
#     gdma_ll_rx_enable_interrupt(..., false);
#     gdma_ll_rx_stop(data->hal.dev, dma_channel->channel_id);
#     return 0;
#
# It masks the interrupt, writes the STOP bit, and returns. STOP is a REQUEST. On an
# ESP32-S3 the channel goes on writing into the buffer after dma_stop() has returned 0 --
# measured, not inferred: a canary that stamps every free mem-slab block, hands them back,
# waits 50 ms and takes them again catches 1 of 8 overwritten on every cycle, and the run
# dies with EXCCAUSE 28 inside k_mem_slab_alloc(), walking a free list whose next pointer
# has been overwritten with two int16 audio samples.
#
# WHAT DOES NOT FIX IT
#
#   Waiting for the descriptor FSM's PARK bit -- the bit this driver's own get_status()
#   uses to answer "busy" -- does NOT work. Measured over 890 dma_stop() calls: the FSM
#   was ALREADY IDLE every single time, it never spun once, and the DMA wrote into the
#   freed block anyway. PARK observes "no more descriptors to fetch", not "no more bytes
#   in flight". A wait on it is a fix that fixes nothing while looking like a proof.
#
#   Espressif's own hardware makes the same point: the ESP32-P4 AXI DMA has an ABORT bit
#   ("stop the undergoing transfer immediately") and an "is reset available" status, both
#   distinct from stop and from park. The S3's AHB GDMA has neither. There is no
#   quiescence observable to poll for.
#
# WHAT DOES
#
#   Reset the channel. gdma_ll_rx_reset_channel() is "Reset DMA RX channel FSM and FIFO
#   pointer" -- and Espressif, on an open ESP32-S3 GDMA bug report about exactly this
#   class of problem: "gdma_reset won't reset registers. It only reset the FIFO and FSM."
#   The FIFO is the state PARK cannot see, and clearing it is what stops the writes.
#
#   Measured: with the reset in dma_stop() and NOTHING else changed -- the I2S RX unit
#   left running, no peripheral stop at all -- 100 cycles pass with the canary clean and
#   the mem-slab flat.
#
#   It is safe across a stop/start: dma_esp32_start() re-arms the descriptor address and
#   the interrupt enable, and the descriptor list itself lives in RAM and is not touched
#   by a channel reset. The driver already resets the channel in dma_esp32_config_*() and
#   dma_esp32_reload(), so this is the same primitive it uses to arm a transfer, applied
#   where it disarms one.
#
# This is a client-independent fix. Every ESP32 DMA client that frees or reuses a buffer
# after dma_stop() has the same exposure; the I2S driver is simply the one that was caught.
#
# Usage: bash scripts/patch_zephyr_dma_quiesce.sh <zephyr-tree>
set -euo pipefail

Z=${1:?usage: patch_zephyr_dma_quiesce.sh <zephyr-tree>}
F=$Z/drivers/dma/dma_esp32_gdma.c

[ -f "$F" ] || { echo "not a Zephyr tree (no $F)" >&2; exit 1; }

python3 - "$F" <<'PY'
import io
import sys

path = sys.argv[1]
src = io.open(path, encoding="utf-8", newline="\n").read()

RESET_COMMENT = """
	/*
	 * Writing the STOP bit only asks the channel to stop; it goes on writing into the
	 * caller's buffer after this function has returned. Reset it -- "Reset DMA RX
	 * channel FSM and FIFO pointer" -- so that dma_stop() means what the API says it
	 * means and the buffer really can be reused.
	 *
	 * The descriptor FSM's PARK bit is NOT sufficient and must not be used here: it
	 * reads idle while the data path is still draining. Only the FIFO reset stops the
	 * writes.
	 *
	 * dma_esp32_start() re-arms the descriptor address and the interrupt enable, and
	 * the descriptor list lives in RAM, so a reset here costs a restarted transfer
	 * nothing. This driver already resets the channel to ARM a transfer, in
	 * dma_esp32_config_*() and dma_esp32_reload().
	 */"""

# The driver exists in two forms and the patch has to fit both. v4.4.0 calls the LL
# directly; upstream main was refactored onto a gdma_hal_* wrapper. They are the same
# register write either way -- gdma_hal_reset() -> hal->reset() -> gdma_ahb_hal_reset()
# -> ahb_dma_ll_rx_reset_channel(), which is gdma_ll_rx_reset_channel() renamed -- so the
# hardware validation of one carries to the other.
FORMS = [
    ("main (gdma_hal_*)", "gdma_hal_reset", [
        ("""		gdma_hal_stop(&data->hal, dma_channel->channel_id, GDMA_CHANNEL_DIRECTION_RX);
		gdma_hal_stop(&data->hal, dma_channel->channel_id, GDMA_CHANNEL_DIRECTION_TX);
#if CONFIG_PM""",
         """		gdma_hal_stop(&data->hal, dma_channel->channel_id, GDMA_CHANNEL_DIRECTION_RX);
		gdma_hal_stop(&data->hal, dma_channel->channel_id, GDMA_CHANNEL_DIRECTION_TX);
		gdma_hal_reset(&data->hal, dma_channel->channel_id, GDMA_CHANNEL_DIRECTION_RX);
		gdma_hal_reset(&data->hal, dma_channel->channel_id, GDMA_CHANNEL_DIRECTION_TX);
#if CONFIG_PM"""),
        ("""	if (dma_channel->dir == DMA_RX) {
		gdma_hal_enable_intr(&data->hal, dma_channel->channel_id, GDMA_CHANNEL_DIRECTION_RX,
				     GDMA_LL_RX_EVENT_MASK, false);
		gdma_hal_stop(&data->hal, dma_channel->channel_id, GDMA_CHANNEL_DIRECTION_RX);
	} else if (dma_channel->dir == DMA_TX) {
		gdma_hal_enable_intr(&data->hal, dma_channel->channel_id, GDMA_CHANNEL_DIRECTION_TX,
				     GDMA_LL_TX_EVENT_MASK, false);
		gdma_hal_stop(&data->hal, dma_channel->channel_id, GDMA_CHANNEL_DIRECTION_TX);
	}

	return 0;
}""",
         RESET_COMMENT + """
	if (dma_channel->dir == DMA_RX) {
		gdma_hal_enable_intr(&data->hal, dma_channel->channel_id, GDMA_CHANNEL_DIRECTION_RX,
				     GDMA_LL_RX_EVENT_MASK, false);
		gdma_hal_stop(&data->hal, dma_channel->channel_id, GDMA_CHANNEL_DIRECTION_RX);
		gdma_hal_reset(&data->hal, dma_channel->channel_id, GDMA_CHANNEL_DIRECTION_RX);
	} else if (dma_channel->dir == DMA_TX) {
		gdma_hal_enable_intr(&data->hal, dma_channel->channel_id, GDMA_CHANNEL_DIRECTION_TX,
				     GDMA_LL_TX_EVENT_MASK, false);
		gdma_hal_stop(&data->hal, dma_channel->channel_id, GDMA_CHANNEL_DIRECTION_TX);
		gdma_hal_reset(&data->hal, dma_channel->channel_id, GDMA_CHANNEL_DIRECTION_TX);
	}

	return 0;
}"""),
    ]),
    ("v4.4.0 (gdma_ll_*)", "gdma_ll_rx_reset_channel", [
        ("""		gdma_ll_rx_stop(data->hal.dev, dma_channel->channel_id);
		gdma_ll_tx_stop(data->hal.dev, dma_channel->channel_id);
#if CONFIG_PM""",
         """		gdma_ll_rx_stop(data->hal.dev, dma_channel->channel_id);
		gdma_ll_tx_stop(data->hal.dev, dma_channel->channel_id);
		gdma_ll_rx_reset_channel(data->hal.dev, dma_channel->channel_id);
		gdma_ll_tx_reset_channel(data->hal.dev, dma_channel->channel_id);
#if CONFIG_PM"""),
        ("""	if (dma_channel->dir == DMA_RX) {
		gdma_ll_rx_enable_interrupt(data->hal.dev, dma_channel->channel_id,
					    GDMA_LL_RX_EVENT_MASK, false);
		gdma_ll_rx_stop(data->hal.dev, dma_channel->channel_id);
	} else if (dma_channel->dir == DMA_TX) {
		gdma_ll_tx_enable_interrupt(data->hal.dev, dma_channel->channel_id,
					    GDMA_LL_TX_EVENT_MASK, false);
		gdma_ll_tx_stop(data->hal.dev, dma_channel->channel_id);
	}

	return 0;
}""",
         RESET_COMMENT + """
	if (dma_channel->dir == DMA_RX) {
		gdma_ll_rx_enable_interrupt(data->hal.dev, dma_channel->channel_id,
					    GDMA_LL_RX_EVENT_MASK, false);
		gdma_ll_rx_stop(data->hal.dev, dma_channel->channel_id);
		gdma_ll_rx_reset_channel(data->hal.dev, dma_channel->channel_id);
	} else if (dma_channel->dir == DMA_TX) {
		gdma_ll_tx_enable_interrupt(data->hal.dev, dma_channel->channel_id,
					    GDMA_LL_TX_EVENT_MASK, false);
		gdma_ll_tx_stop(data->hal.dev, dma_channel->channel_id);
		gdma_ll_tx_reset_channel(data->hal.dev, dma_channel->channel_id);
	}

	return 0;
}"""),
    ]),
]


# Count the resets INSIDE dma_esp32_stop() only. Both driver forms already reset the
# channel in config_*() and reload(), so a bare name grep finds those and calls an
# untouched tree "patched" -- which is exactly the mistake that made me report a
# channel-reset experiment I had never actually run.
def in_stop(text):
    i = text.find("static int dma_esp32_stop(")
    if i < 0:
        return 0
    j = text.find("\n}\n", i)
    body = text[i:j]
    return body.count("reset_channel") + body.count("gdma_hal_reset")


if in_stop(src) > 0:
    print("  already patched; nothing to do")
    raise SystemExit(0)

form = None
for name, _marker, pairs in FORMS:
    if all(src.count(old) == 1 for old, _new in pairs):
        form = (name, pairs)
        break

if form is None:
    raise SystemExit("  dma_esp32_stop() matches neither the v4.4.0 nor the upstream-main "
                     "form of this driver. Re-derive the patch against the tree you have.")

name, pairs = form
for old, new in pairs:
    src = src.replace(old, new, 1)

io.open(path, "w", encoding="utf-8", newline="\n").write(src)

n = in_stop(src)
if n != 4:
    raise SystemExit("  applied, but dma_esp32_stop() has %d resets and should have 4" % n)

print("  patched drivers/dma/dma_esp32_gdma.c  [%s]" % name)
print("    dma_esp32_stop(): reset the channel (FSM + FIFO) after the STOP command, so")
print("                      the buffer really is safe to reuse when this returns 0")
PY
