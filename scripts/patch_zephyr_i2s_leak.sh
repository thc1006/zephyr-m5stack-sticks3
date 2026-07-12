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
# Because on the SOC_GDMA_SUPPORTED path, *_stop_transfer() calls dma_stop() and
# NOTHING ELSE. It never stops the I2S unit that is feeding the DMA -- unlike the
# non-GDMA branch three lines below it, which stops the link, disables the DMA and
# clears the interrupt status. THE LEAK HAS BEEN MASKING THAT: the block was never
# given back, so the stray writes landed in memory nobody would ever look at again.
# Hand it back and the hardware writes into the free list.
#
# THE FIX, in three parts. Order matters.
#
#   1. Quiesce the hardware. On the GDMA path, stop the I2S unit (i2s_hal_rx_stop /
#      i2s_hal_tx_stop) before dma_stop(), and clear dma_pending. Now the DMA really
#      is finished with the buffer.
#   2. i2s_esp32_tx_callback(): NULL mem_block after freeing it. The RX callback
#      already does; without this, part 3 is a double free on the tx_disable path.
#   3. *_stop_transfer(): return the in-flight block to the slab.
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

TX_Q_OLD = """	const struct i2s_esp32_stream *stream = &dev_cfg->tx;

#if SOC_GDMA_SUPPORTED
	dma_stop(stream->conf->dma_dev, stream->conf->dma_channel);
#else
	const i2s_hal_context_t *hal = &(dev_cfg->hal);

	esp_intr_disable(stream->data->irq_handle);"""
TX_Q_NEW = """	const struct i2s_esp32_stream *stream = &dev_cfg->tx;
	const i2s_hal_context_t *hal = &(dev_cfg->hal);

#if SOC_GDMA_SUPPORTED
	i2s_hal_tx_stop(hal);
	dma_stop(stream->conf->dma_dev, stream->conf->dma_channel);
#else
	esp_intr_disable(stream->data->irq_handle);"""

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

# Already patched? Test for the parts that survive the LATER rewrites. STOP_NEW does
# not: the dma_pending line gets inserted into the middle of it, so checking for it
# here made a second run fail with "anchor matched 0 times" instead of a no-op.
if src.count("i2s_hal_rx_stop(hal);") and src.count("i2s_hal_tx_stop(hal);") and \
        src.count("stream->data->dma_pending = false;\n\tstream->data->transferring") == 2:
    print("  already patched; nothing to do")
    raise SystemExit(0)

for name, old, want in (("rx quiesce", RX_Q_OLD, 1),
                        ("tx quiesce", TX_Q_OLD, 1),
                        ("stop_transfer", STOP_OLD, 2),
                        ("tx_callback", TX_CB_OLD, 1)):
    if src.count(old) != want:
        raise SystemExit("  the %s anchor matched %d times, expected %d. The driver has "
                         "changed; re-derive the patch." % (name, src.count(old), want))

# PENDING_OLD is a SUBSTRING of STOP_OLD, so the stop-transfer rewrite has to land
# first or the second anchor stops matching. Getting this order wrong silently patches
# only one of the two directions.
src = src.replace(RX_Q_OLD, RX_Q_NEW)
src = src.replace(TX_Q_OLD, TX_Q_NEW)
src = src.replace(STOP_OLD, STOP_NEW)
src = src.replace(PENDING_OLD, PENDING_NEW)
src = src.replace(TX_CB_OLD, TX_CB_NEW)

io.open(path, "w", encoding="utf-8", newline="\n").write(src)
print("  patched drivers/i2s/i2s_esp32.c")
print("    {rx,tx}_stop_transfer: stop the I2S unit before dma_stop(), clear dma_pending,")
print("                           and THEN return the in-flight block")
print("    tx_callback:           NULL mem_block after the free (or the above double-frees)")
PY
