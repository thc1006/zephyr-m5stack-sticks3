#!/usr/bin/env bash
# Copyright (c) 2026 Hsiu-Chi Tsai
# SPDX-License-Identifier: Apache-2.0
#
# Fix the mem-slab leak in Zephyr's ESP32 I2S driver.
#
# THE BUG. drivers/i2s/i2s_esp32.c leaks one mem-slab block per direction on every
# START/DROP cycle. i2s_esp32_rx_stop_transfer() and i2s_esp32_tx_stop_transfer()
# set stream->data->mem_block -- the block the DMA is working on -- to NULL without
# returning it to the slab, and I2S_TRIGGER_DROP calls exactly those. The queue purge
# that follows (i2s_esp32_queue_drop) only frees what is sitting in the message queue.
#
# WHY IT IS A HANG AND NOT AN ERROR. i2s_buf_write() allocates its block with
# K_FOREVER (drivers/i2s/i2s_common.c) -- the i2s_config timeout does not apply to
# that allocation -- so an exhausted slab is not -ENOMEM, it is an unkillable block.
# No error, no fault, no log line. The application simply stops.
#
# MEASURED, on an ESP32-S3-PICO-1 (M5StickS3), BLOCK_COUNT = 8, one reconfigure per
# sample rate (evidence/20260712-hw019-i2s-slab-leak-diagnostic.log):
#
#     rate       8k  11k  12k  16k  22k  24k  32k  44.1k
#     tx free     8    7    6    5    4    3    2      1   -> i2s_buf_write() hangs
#     rx free     8    7    6    5    4    3    2      1
#
#   and the DROP -> PREPARE -> DROP that is supposed to recover them recovers none.
#   With this patch the census reads 8/8 at every rate and the sweep completes
#   (evidence/20260712-hw019-es8311-rate-sweep-PASS.log).
#
# WHO IT HITS. Any ESP32 application that changes I2S sample rate, or stops and
# restarts a stream, more than BLOCK_COUNT times. That includes this project's own
# audio app: entering and leaving the AUDIO page eight times exhausts the slab, and
# the ninth capture hangs forever.
#
# THE FIX, in three hunks. Part 1 must land first: the TX callback frees mem_block
# without NULLing it (the RX callback does NULL it), so freeing in stop_transfer
# without part 1 would be a double free on the tx_disable path.
#
#   1. i2s_esp32_tx_callback():        NULL mem_block after freeing it.
#   2. i2s_esp32_rx_stop_transfer():   free the in-flight block before dropping it.
#   3. i2s_esp32_tx_stop_transfer():   likewise.
#
# Present in Zephyr v4.4.0 and on main as of 2026-07-12. Idempotent: re-running is a
# no-op.
#
# Usage: bash scripts/patch_zephyr_i2s_leak.sh <zephyr-tree>
set -euo pipefail

Z=${1:?usage: patch_zephyr_i2s_leak.sh <zephyr-tree>}
F=$Z/drivers/i2s/i2s_esp32.c

[ -f "$F" ] || { echo "not a Zephyr tree (no $F)" >&2; exit 1; }

python3 - "$F" <<'PY'
import io, sys

path = sys.argv[1]
src = io.open(path, encoding="utf-8", newline="\n").read()

# Part 1: the TX callback must NULL mem_block after freeing it.
old1 = """	k_mem_slab_free(stream->data->i2s_cfg.mem_slab, stream->data->mem_block);

#if SOC_GDMA_SUPPORTED
	if (status < 0) {"""
new1 = """	k_mem_slab_free(stream->data->i2s_cfg.mem_slab, stream->data->mem_block);
	stream->data->mem_block = NULL;
	stream->data->mem_block_len = 0;

#if SOC_GDMA_SUPPORTED
	if (status < 0) {"""

# Parts 2 and 3: both *_stop_transfer() must return the in-flight block.
old2 = """	stream->data->mem_block = NULL;
	stream->data->mem_block_len = 0;

	stream->data->transferring = false;
}"""
new2 = """	if (stream->data->mem_block != NULL) {
		k_mem_slab_free(stream->data->i2s_cfg.mem_slab, stream->data->mem_block);
		stream->data->mem_block = NULL;
	}
	stream->data->mem_block_len = 0;

	stream->data->transferring = false;
}"""

if src.count(new2) == 2 and src.count(new1) == 1:
    print("  already patched; nothing to do")
    raise SystemExit(0)

if src.count(old1) != 1:
    raise SystemExit("  the tx_callback anchor does not match (%d hits). The driver has "
                     "changed; re-derive the patch." % src.count(old1))
if src.count(old2) != 2:
    raise SystemExit("  the stop_transfer anchor matched %d times, expected 2 (rx + tx). "
                     "The driver has changed; re-derive the patch." % src.count(old2))

src = src.replace(old1, new1).replace(old2, new2)
io.open(path, "w", encoding="utf-8", newline="\n").write(src)
print("  patched drivers/i2s/i2s_esp32.c")
print("    tx_callback:      NULL mem_block after the free")
print("    rx_stop_transfer: free the in-flight block")
print("    tx_stop_transfer: free the in-flight block")
PY
