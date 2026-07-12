#!/usr/bin/env bash
# Copyright (c) 2026 Hsiu-Chi Tsai
# SPDX-License-Identifier: Apache-2.0
#
# Fix the mem-slab leak in Zephyr's ESP32 I2S driver -- and the reason the obvious
# fix for it does not work.
#
# THE LEAK. drivers/i2s/i2s_esp32.c loses one mem-slab block per direction on every
# START/DROP cycle. i2s_esp32_rx_stop_transfer() and i2s_esp32_tx_stop_transfer()
# set stream->data->mem_block -- the block the DMA is working on -- to NULL without
# returning it to the slab, and I2S_TRIGGER_DROP calls exactly those. The queue purge
# that follows only frees what is sitting in the message queue.
#
# WHY IT IS A HANG AND NOT AN ERROR. i2s_buf_write() allocates its block with
# K_FOREVER (drivers/i2s/i2s_common.c) -- the i2s_config timeout does not apply to
# that allocation -- so an exhausted slab is not -ENOMEM, it is an unkillable block.
# No error, no fault, no log line. The application simply stops.
#
# Measured on an ESP32-S3-PICO-1, BLOCK_COUNT = 8, one reconfigure per cycle
# (evidence/20260712-hw021-i2s-slab-stock.log):
#
#     cycle      1  2  3  4  5  6  7  8
#     tx free    8  7  6  5  4  3  2  -> i2s_buf_write() hangs forever
#     rx free    8  7  6  5  4  3  2  1
#
# WHY YOU CANNOT JUST FREE THE BLOCK. This is the part that cost a withdrawn upstream
# pull request, and it is the whole reason this script exists rather than a two-line
# patch. Simply returning the in-flight block in *_stop_transfer() CRASHES within two
# cycles (evidence/20260712-hw021-i2s-crash-on-naive-fix.log):
#
#     FATAL EXCEPTION: EXCCAUSE 28 (load prohibited)
#     PC   k_mem_slab_alloc      (kernel/mem_slab.c:245)
#     from i2s_esp32_rx_callback (drivers/i2s/i2s_esp32.c)
#     VADDR 0x220022   <- 0x0022, 0x0022: two int16 audio samples, not a pointer
#
# mem_slab.c:245 is `slab->free_list = *(char **)(slab->free_list);`. The free list's
# next pointer has been overwritten with captured audio. A canary measures it exactly:
# stamp all eight free RX blocks, hand them back, wait 50 ms, and take them again --
# exactly ONE comes back with a stray write in it, every cycle. The DMA is still
# writing into memory the driver has already returned to the slab.
#
# Because on the SOC_GDMA_SUPPORTED path, i2s_esp32_rx_stop_transfer() calls dma_stop()
# and NOTHING ELSE. It never stops the I2S RX unit that is feeding the DMA, so the unit
# goes on pushing samples into the FIFO and the channel goes on draining them into the
# buffer for a short while after dma_stop() returns. THE LEAK HAS BEEN MASKING THAT: the
# block was never given back, so the stray writes landed in memory nobody would ever look
# at again. Hand it back and the hardware writes into the free list.
#
# ONLY RX. This is a direction-asymmetric bug and the fix has to be asymmetric too. The
# RX DMA WRITES to the mem-slab block, so a burst that lands after the block is freed
# corrupts the free list -- that is the crash. The TX DMA only READS from it, so a late
# read of a freed block is harmless: it cannot move a single byte of slab metadata.
#
# And stopping the TX unit is not merely unnecessary, it is actively wrong: on this SoC
# TX and RX share the bit clock, and the I2S TX unit is what generates it. An earlier
# version of this patch called i2s_hal_tx_stop() as well, "for symmetry". Measured: on a
# full-duplex graceful STOP the TX side stops first, the bit clock dies with it, the RX
# DMA can then never finish the block it is holding, i2s_esp32_rx_callback() never fires
# again, and the stream is stranded in I2S_STATE_STOPPING forever. Stock Zephyr drains a
# tail block there and recovers; that build drained nothing, every time. A hundred
# START/DROP cycles could not see it, because DROP never enters STOPPING at all.
#
# THE FIX, in four parts. Order matters.
#
#   1. Quiesce the RX hardware. On the GDMA path, stop the I2S RX unit (i2s_hal_rx_stop)
#      before dma_stop(), and clear dma_pending in both directions. Now the DMA really
#      is finished with the buffer. TX is deliberately left alone -- see above.
#   2. i2s_esp32_tx_callback(): NULL mem_block after freeing it. Without this, part 3
#      is a double free on the tx_disable path.
#   3. *_stop_transfer(): return the in-flight block to the slab.
#   4. i2s_esp32_rx_callback(): NULL mem_block once the QUEUE owns the block. Without
#      this, part 3 is a double free on the graceful-stop path -- see below.
#
# PART 3 CREATED A SECOND BUG AND PART 4 IS THE OTHER HALF OF IT. Giving
# *_stop_transfer() the job of returning whatever mem_block points at is right for a
# block the DMA still owns and wrong for one the RX callback has already handed to the
# receive queue, because mem_block goes on pointing at it. The STOPPING branch six lines
# below that k_msgq_put() does `goto rx_disable`, which lands in rx_stop_transfer(),
# which frees the block the queue is holding; i2s_read() then returns a block that is
# already back on the free list and frees it again. The leak had been covering for this
# one too. The stress test below could not see it because it only ever used DROP, which
# never reaches I2S_STATE_STOPPING.
#
# Verified on hardware (evidence/20260712-hw021-i2s-slab-quiesce-PASS.log): 100
# reconfigure/START/DROP cycles, 20 of them with TX deliberately starved to drive the
# driver down its tx_disable path, the slab flat at 8/8 free the whole way, and the
# canary clean -- 0 of 8 blocks touched after being handed back.
#
# Present in Zephyr v4.4.0 and on main as of 2026-07-12. Idempotent.
#
# Usage: bash scripts/patch_zephyr_i2s_leak.sh <zephyr-tree>
set -euo pipefail

Z=${1:?usage: patch_zephyr_i2s_leak.sh <zephyr-tree>}
F=$Z/drivers/i2s/i2s_esp32.c

[ -f "$F" ] || { echo "not a Zephyr tree (no $F)" >&2; exit 1; }

python3 - "$F" <<'PY'
import io
import sys

path = sys.argv[1]
src = io.open(path, encoding="utf-8", newline="\n").read()

# --- part 1: quiesce the I2S unit, not just the DMA channel ---
RX_Q_OLD = """	const struct i2s_esp32_stream *stream = &dev_cfg->rx;

#if SOC_GDMA_SUPPORTED
	dma_stop(stream->conf->dma_dev, stream->conf->dma_channel);
#else
	const i2s_hal_context_t *hal = &(dev_cfg->hal);

	esp_intr_disable(stream->data->irq_handle);"""
RX_Q_NEW = """	const struct i2s_esp32_stream *stream = &dev_cfg->rx;
	const i2s_hal_context_t *hal = &(dev_cfg->hal);

#if SOC_GDMA_SUPPORTED
	i2s_hal_rx_stop(hal);
	dma_stop(stream->conf->dma_dev, stream->conf->dma_channel);
#else
	esp_intr_disable(stream->data->irq_handle);"""

# There is deliberately no TX counterpart. i2s_esp32_tx_stop_transfer() keeps calling
# dma_stop() alone, because the TX DMA only reads from the block and because the I2S TX
# unit is the source of the shared bit clock -- stopping it strands a full-duplex RX
# stream in I2S_STATE_STOPPING with a block it can never finish.

# --- part 3: return the in-flight block ---
STOP_OLD = """	stream->data->mem_block = NULL;
	stream->data->mem_block_len = 0;

	stream->data->transferring = false;
}"""
STOP_NEW = """	if (stream->data->mem_block != NULL) {
		k_mem_slab_free(stream->data->i2s_cfg.mem_slab, stream->data->mem_block);
		stream->data->mem_block = NULL;
	}
	stream->data->mem_block_len = 0;

	stream->data->transferring = false;
}"""

# --- part 1b: clear dma_pending (a stale latched completion must not be believed) ---
PENDING_OLD = """	stream->data->mem_block_len = 0;

	stream->data->transferring = false;
}"""
PENDING_NEW = """	stream->data->mem_block_len = 0;

	stream->data->dma_pending = false;
	stream->data->transferring = false;
}"""

# --- part 2: the TX callback must NULL mem_block after freeing it ---
TX_CB_OLD = """	k_mem_slab_free(stream->data->i2s_cfg.mem_slab, stream->data->mem_block);

#if SOC_GDMA_SUPPORTED
	if (status < 0) {"""
TX_CB_NEW = """	k_mem_slab_free(stream->data->i2s_cfg.mem_slab, stream->data->mem_block);
	stream->data->mem_block = NULL;
	stream->data->mem_block_len = 0;

#if SOC_GDMA_SUPPORTED
	if (status < 0) {"""

# --- part 4: the RX callback must let go of a block it has handed to the queue ---
#
# Part 3 gave *_stop_transfer() the job of returning whatever mem_block points at. That
# is right for a block the DMA still owns, and WRONG for one the callback has already
# put in the receive queue -- because mem_block still points at it. Six lines below the
# k_msgq_put(), the STOPPING branch does `goto rx_disable`, which lands in
# rx_stop_transfer(), which now frees the block the queue is holding. i2s_read() then
# hands the caller a block that is back on the free list, and frees it a second time.
#
# So part 3 turned a leak into a double free on the graceful-stop path. The leak had
# been covering for this too. Ownership has to be handed over explicitly: the pointer
# goes NULL at the instant the queue takes the block, and every path out of the callback
# is then correct by construction rather than by accident --
#
#   queue full     mem_block still ours       -> rx_stop_transfer() frees it   (right)
#   STOPPING       mem_block NULL, queue owns -> rx_stop_transfer() skips it   (right)
#   alloc failed   k_mem_slab_alloc() NULLs it-> rx_stop_transfer() skips it   (right)
#   dma failed     freed and NULLed inline    -> rx_stop_transfer() skips it   (right)
#
# The alloc-failure row is the one worth spelling out: it is safe TODAY only because
# k_mem_slab_alloc() writes NULL through its out-parameter when a K_NO_WAIT allocation
# fails (kernel/mem_slab.c). That is an implementation detail of the allocator, not a
# documented contract, and relying on it is how this class of bug gets rebuilt. With the
# hand-off below, that path does not need it.
RX_CB_OLD = """	err = k_msgq_put(&stream->data->queue, &item, K_NO_WAIT);
	if (err < 0) {
		LOG_DBG("RX queue full");
		dev_data->state = I2S_STATE_ERROR;
		goto rx_disable;
	}

	if (dev_data->state == I2S_STATE_STOPPING) {"""
RX_CB_NEW = """	err = k_msgq_put(&stream->data->queue, &item, K_NO_WAIT);
	if (err < 0) {
		LOG_DBG("RX queue full");
		dev_data->state = I2S_STATE_ERROR;
		goto rx_disable;
	}

	/*
	 * The queue owns the block now and i2s_read() will free it, so the driver must
	 * stop pointing at it. Every path out of this callback that stops the stream ends
	 * in i2s_esp32_rx_stop_transfer(), which frees whatever mem_block still holds --
	 * and on the STOPPING branch immediately below, that would be this block, freed a
	 * second time while the queue is still holding it.
	 */
	stream->data->mem_block = NULL;
	stream->data->mem_block_len = 0;

	if (dev_data->state == I2S_STATE_STOPPING) {"""

# Already patched? Test for the parts that survive the LATER rewrites. STOP_NEW does
# not: the dma_pending line gets inserted into the middle of it, so checking for it
# here made a second run fail with "anchor matched 0 times" instead of a no-op.
# The unit-stop calls have to be matched TOGETHER WITH the dma_stop() line beneath them.
# Both i2s_hal_rx_stop() and i2s_hal_tx_stop() already appear in the pristine driver -- in
# *_start_transfer(), which stops and resets the unit before arming the next transfer --
# so a bare name test says "already patched" about a tree that is untouched.
RX_QUIESCED = "\ti2s_hal_rx_stop(hal);\n\tdma_stop(stream->conf->dma_dev, stream->conf->dma_channel);"
TX_QUIESCED = "\ti2s_hal_tx_stop(hal);\n\tdma_stop(stream->conf->dma_dev, stream->conf->dma_channel);"

if src.count(RX_QUIESCED) == 1 and \
        src.count("stream->data->dma_pending = false;\n\tstream->data->transferring") == 2 and \
        src.count("The queue owns the block now") == 1:
    print("  already patched; nothing to do")
    raise SystemExit(0)

# The TX unit must NOT be stopped in tx_stop_transfer(). An earlier version of this script
# did exactly that, "for symmetry", and it kills the shared bit clock.
if TX_QUIESCED in src:
    raise SystemExit("  this tree has i2s_hal_tx_stop() in tx_stop_transfer(). That kills "
                     "the shared bit clock and strands full-duplex RX in STOPPING forever. "
                     "Restore drivers/i2s/i2s_esp32.c and re-run.")

for name, old, want in (("rx quiesce", RX_Q_OLD, 1),
                        ("stop_transfer", STOP_OLD, 2),
                        ("tx_callback", TX_CB_OLD, 1),
                        ("rx_callback", RX_CB_OLD, 1)):
    if src.count(old) != want:
        raise SystemExit("  the %s anchor matched %d times, expected %d. The driver has "
                         "changed; re-derive the patch." % (name, src.count(old), want))

# PENDING_OLD is a SUBSTRING of STOP_OLD, so the stop-transfer rewrite has to land
# first or the second anchor stops matching. Getting this order wrong silently patches
# only one of the two directions.
src = src.replace(RX_Q_OLD, RX_Q_NEW)
src = src.replace(STOP_OLD, STOP_NEW)
src = src.replace(PENDING_OLD, PENDING_NEW)
src = src.replace(TX_CB_OLD, TX_CB_NEW)
src = src.replace(RX_CB_OLD, RX_CB_NEW)

io.open(path, "w", encoding="utf-8", newline="\n").write(src)
print("  patched drivers/i2s/i2s_esp32.c")
print("    rx_stop_transfer: stop the I2S RX UNIT before dma_stop() -- the RX DMA writes")
print("                      to the block, and a late burst lands on the free list")
print("    tx_stop_transfer: unchanged. The TX DMA only reads, and the TX unit is the")
print("                      source of the shared bit clock.")
print("    both:             clear dma_pending, and THEN return the in-flight block")
print("    tx_callback:      NULL mem_block after the free (or the above double-frees)")
print("    rx_callback:      NULL mem_block once the queue owns it (or stop_transfer")
print("                      double-frees it on the graceful-stop path)")
PY
