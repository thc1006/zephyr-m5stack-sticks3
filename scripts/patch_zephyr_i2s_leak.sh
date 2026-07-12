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
# which frees the block the queue is holding. i2s_read() then hands that block to its
# caller with it already back on the free list, and the caller frees it a SECOND time.
# (i2s_read() itself does NOT free: it transfers ownership. i2s_buf_read() frees on the
# caller's behalf; an application calling i2s_read() directly owns the block and must
# free it itself.) The leak had been covering for this one too, and the stress could not
# see it because it only ever used DROP, which never reaches I2S_STATE_STOPPING.
#
# Verified on hardware (evidence/20260712-hw024-*): 100 reconfigure/START/DROP cycles,
# 20 of them with TX deliberately starved to drive the driver down its tx_disable path,
# and 27 of them ending in a graceful I2S_TRIGGER_STOP with a tail read instead of a
# DROP. Slab flat at 8/8 free the whole way, all 27 STOPs drained their tail block, and
# the canary clean -- 0 of 8 blocks touched after being handed back.
#
# The harness fails on stock (the slab drains), on the i2s_hal_tx_stop variant (27 of 27
# STOPs drain nothing, with the slab still flat), and with part 4 removed (the slab
# climbs to 9 free blocks out of 8). It did NOT fail on the tx_stop variant until
# 2026-07-13: it printed "STOP drained 0 tail blocks" and then printed STRESS PASSED,
# because nothing asserted on that number. A test that prints the evidence and does not
# check it is not a test.
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

# --- part 1: stop the PRODUCER before the consumer, on BOTH paths ---
#
# i2s_hal_rx_stop() appears exactly once in this driver, in i2s_esp32_rx_start_transfer().
# It is called NOWHERE in the stop path -- not on the GDMA branch and not on the non-GDMA
# one. So the I2S RX unit goes on clocking samples into the FIFO while the DMA is being
# told to stop, and on the GDMA branch that is enough to overwrite a mem-slab free list
# with captured audio, measured.
#
# ESP-IDF's own i2s_rx_stop() calls i2s_hal_rx_stop() FIRST, above its own #if/#else, on
# both branches. Zephyr skipped it on both. That is one omission, not two, and it is
# fixed here in one place.
#
# The non-GDMA branch needs more than that, and Espressif's code says exactly what:
#
#     static void i2s_rx_reset(i2s_port_t i2s_num) {
#     #if SOC_GDMA_SUPPORTED
#             gdma_reset(p_i2s[i2s_num]->rx_dma_chan);
#     #else
#             i2s_hal_rx_reset_dma(&(p_i2s[i2s_num]->hal));
#     #endif
#
# i2s_hal_rx_reset_dma() -- I2S_IN_RST, "Set this bit to reset in DMA FSM" -- is the
# non-GDMA analogue of the gdma_reset() this series adds to dma_stop(). Espressif pairs
# them in the same #if/#else. It is added here for the same reason.
#
# And i2s_hal_rx_disable_dma() is NOT a stop, which is worth writing down because it looks
# like one:
#
#     #define i2s_hal_rx_disable_dma(hal) i2s_ll_enable_dma((hal)->dev, false)
#     static inline void i2s_ll_enable_dma(i2s_dev_t *hw, bool ena) {
#             hw->fifo_conf.dscr_en = ena;
#     }
#
# I2S_DSCR_EN is "Set this bit to enable I2S DMA mode" (ESP32 TRM p.439). A mode select.
# It promises neither that no new burst starts nor that an in-flight one retires.
#
# dma_stop()'s return value stops being decorative here. ESP32's implementation can only
# fail with -EINVAL on a bad channel index today, so the branch is unreachable in
# practice -- but the entire point of this patch is that a block may only go back to the
# slab once the hardware has let go of it, and that has to be a CONSEQUENCE of the stop
# succeeding, not a thing done next to it. A leaked block is a slow bleed; a block the DMA
# is still writing to is a crash.
RX_Q_OLD = """	const struct i2s_esp32_stream *stream = &dev_cfg->rx;

#if SOC_GDMA_SUPPORTED
	dma_stop(stream->conf->dma_dev, stream->conf->dma_channel);
#else
	const i2s_hal_context_t *hal = &(dev_cfg->hal);

	esp_intr_disable(stream->data->irq_handle);
	i2s_hal_rx_stop_link(hal);
	i2s_hal_rx_disable_intr(hal);
	i2s_hal_rx_disable_dma(hal);
	i2s_hal_clear_intr_status(hal, I2S_INTR_MAX);
#endif /* SOC_GDMA_SUPPORTED */"""
RX_Q_NEW = """	const struct i2s_esp32_stream *stream = &dev_cfg->rx;
	const i2s_hal_context_t *hal = &(dev_cfg->hal);
	int err;

	/*
	 * The producer, then the consumer -- and on BOTH paths. This call was missing from
	 * the stop path entirely; it appears only in rx_start_transfer(). A DMA that is
	 * told to stop while the I2S unit is still filling the FIFO does not stop writing.
	 * ESP-IDF's i2s_rx_stop() calls this first, above its own #if/#else.
	 */
	i2s_hal_rx_stop(hal);

#if SOC_GDMA_SUPPORTED
	err = dma_stop(stream->conf->dma_dev, stream->conf->dma_channel);
#else
	err = 0;
	esp_intr_disable(stream->data->irq_handle);
	i2s_hal_rx_stop_link(hal);
	i2s_hal_rx_disable_intr(hal);
	i2s_hal_rx_disable_dma(hal);
	/*
	 * The non-GDMA analogue of the gdma_reset() this series adds to dma_stop().
	 * I2S_IN_RST: "Set this bit to reset in DMA FSM." ESP-IDF pairs the two in the same
	 * #if/#else in its own i2s_rx_reset(). Note that i2s_hal_rx_disable_dma() above is
	 * NOT a stop -- it writes I2S_DSCR_EN, "enable I2S DMA mode", a mode select that
	 * promises nothing about a transfer already under way.
	 */
	i2s_hal_rx_reset_dma(hal);
	i2s_hal_clear_intr_status(hal, I2S_INTR_MAX);
#endif /* SOC_GDMA_SUPPORTED */"""

# There is deliberately no TX counterpart. i2s_esp32_tx_stop_transfer() keeps calling
# dma_stop() alone, because the TX DMA only reads from the block and because the I2S TX
# unit is the source of the shared bit clock -- stopping it strands a full-duplex RX
# stream in I2S_STATE_STOPPING with a block it can never finish. TX still gets the error
# check and the ownership rule.
TX_Q_OLD = """	const struct i2s_esp32_stream *stream = &dev_cfg->tx;

#if SOC_GDMA_SUPPORTED
	dma_stop(stream->conf->dma_dev, stream->conf->dma_channel);
#else
	const i2s_hal_context_t *hal = &(dev_cfg->hal);

	esp_intr_disable(stream->data->irq_handle);"""
TX_Q_NEW = """	const struct i2s_esp32_stream *stream = &dev_cfg->tx;
	int err;

#if SOC_GDMA_SUPPORTED
	err = dma_stop(stream->conf->dma_dev, stream->conf->dma_channel);
#else
	const i2s_hal_context_t *hal = &(dev_cfg->hal);

	err = 0;
	esp_intr_disable(stream->data->irq_handle);"""

# --- part 3: return the in-flight block, but ONLY if the channel really stopped ---
STOP_OLD = """	stream->data->mem_block = NULL;
	stream->data->mem_block_len = 0;

	stream->data->transferring = false;
}"""
STOP_NEW = """	stream->data->transferring = false;

	if (err < 0) {
		/*
		 * The channel may still be running, so its block is not ours to give away.
		 * Keeping it leaks one block; handing it back is the free-list corruption
		 * this patch exists to prevent.
		 */
		return;
	}

#if SOC_GDMA_SUPPORTED
	if (stream->data->mem_block != NULL) {
		k_mem_slab_free(stream->data->i2s_cfg.mem_slab, stream->data->mem_block);
		stream->data->mem_block = NULL;
	}
#else
	/*
	 * ESP32 and ESP32-S2 STILL LEAK THIS BLOCK, ON PURPOSE.
	 *
	 * The stop sequence above now matches -- and exceeds -- what ESP-IDF itself does
	 * before it frees this same class of buffer: the I2S unit is stopped first, the
	 * descriptor link is stopped, and the in-DMA FSM is reset. ESP-IDF frees after
	 * that (i2s_set_clk() -> i2s_stop() -> i2s_realloc_dma_buffer()), which is real
	 * evidence that it is enough.
	 *
	 * It is not enough evidence to free the block here, for three reasons.
	 *
	 * The thing that actually owns an in-flight write is the AHB master and its
	 * command FIFO -- I2S_AHBM_RST and I2S_AHBM_FIFO_RST in I2S_LC_CONF_REG -- and
	 * neither has any accessor in the HAL. Nothing above touches them. Nothing can.
	 *
	 * ESP-IDF's free is separated from its stop by a mutex, a state transition and a
	 * log line: microseconds. This one lands nanoseconds later, inside an IRAM_ATTR
	 * function running in ISR context. A window ESP-IDF survives is not a window this
	 * survives.
	 *
	 * And the whole subject of this patch is a stop that looked sufficient and was
	 * not. On the GDMA path that cost a free list overwritten with captured audio, and
	 * it was only found because the hardware was on the desk. Repeating the same
	 * inference on parts nobody in this thread can run would be indefensible. A slow
	 * leak is strictly safer than heap corruption on hardware nobody can see.
	 *
	 * Drop this #if the moment somebody runs it on an ESP32 or an ESP32-S2.
	 */
	stream->data->mem_block = NULL;
#endif /* SOC_GDMA_SUPPORTED */
	stream->data->mem_block_len = 0;
}"""

# --- part 1b: clear dma_pending (a stale latched completion must not be believed) ---
PENDING_OLD = """	stream->data->transferring = false;

	if (err < 0) {"""
PENDING_NEW = """	stream->data->dma_pending = false;
	stream->data->transferring = false;

	if (err < 0) {"""

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
# hands that block to its caller with the block already back on the free list, and the
# caller frees it a second time -- i2s_buf_read() does this for you; an application
# calling i2s_read() directly owns the block and must free it itself.
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
	 * The receive queue owns the block now, and i2s_read() will hand it on to the
	 * application -- which frees it, either directly or inside i2s_buf_read(). Either
	 * way it has stopped being the driver's to release, so the driver must stop
	 * pointing at it. Every path out of this callback that stops the stream ends in
	 * i2s_esp32_rx_stop_transfer(), which frees whatever mem_block still holds -- and
	 * on the STOPPING branch immediately below, that would be this block, freed a
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
RX_QUIESCED = "\ti2s_hal_rx_stop(hal);\n\n#if SOC_GDMA_SUPPORTED\n\terr = dma_stop("
TX_QUIESCED = "\ti2s_hal_tx_stop(hal);\n\tdma_stop(stream->conf->dma_dev, stream->conf->dma_channel);"

if src.count(RX_QUIESCED) == 1 and \
        src.count("stream->data->dma_pending = false;\n\tstream->data->transferring") == 2 and \
        src.count("The receive queue owns the block now") == 1 and \
        src.count("\tif (err < 0) {\n\t\t/*\n\t\t * The channel may still be running") == 2 and \
        src.count("ESP32 and ESP32-S2 STILL LEAK THIS BLOCK") == 2:
    print("  already patched; nothing to do")
    raise SystemExit(0)

# The TX unit must NOT be stopped in tx_stop_transfer(). An earlier version of this script
# did exactly that, "for symmetry", and it kills the shared bit clock.
if TX_QUIESCED in src:
    raise SystemExit("  this tree has i2s_hal_tx_stop() in tx_stop_transfer(). That kills "
                     "the shared bit clock and strands full-duplex RX in STOPPING forever. "
                     "Restore drivers/i2s/i2s_esp32.c and re-run.")

# This script must never be applied on top of scripts/patch_zephyr_dma_park.sh. That one is
# a MEASUREMENT tool -- it proves the park bit is the wrong observable -- and its changes
# must not reach a diff anybody reviews.
if "dma_esp32_wait_park" in io.open(
        path.replace("i2s/i2s_esp32.c", "dma/dma_esp32_gdma.c"),
        encoding="utf-8", newline="\n").read():
    raise SystemExit("  drivers/dma/dma_esp32_gdma.c carries the park-measurement patch. That "
                     "is an experiment, not a fix, and it must not be in a reviewed tree. "
                     "Restore it and re-run.")

for name, old, want in (("rx quiesce", RX_Q_OLD, 1),
                        ("tx quiesce", TX_Q_OLD, 1),
                        ("stop_transfer", STOP_OLD, 2),
                        ("tx_callback", TX_CB_OLD, 1),
                        ("rx_callback", RX_CB_OLD, 1)):
    if src.count(old) != want:
        raise SystemExit("  the %s anchor matched %d times, expected %d. The driver has "
                         "changed; re-derive the patch." % (name, src.count(old), want))

# Order matters twice over. RX_Q/TX_Q introduce the `err` local that STOP_NEW tests, and
# PENDING_OLD only exists once STOP_NEW has been written, so the stop-transfer rewrite has
# to land before it.
src = src.replace(RX_Q_OLD, RX_Q_NEW)
src = src.replace(TX_Q_OLD, TX_Q_NEW)
src = src.replace(STOP_OLD, STOP_NEW)
src = src.replace(PENDING_OLD, PENDING_NEW)
src = src.replace(TX_CB_OLD, TX_CB_NEW)
src = src.replace(RX_CB_OLD, RX_CB_NEW)

io.open(path, "w", encoding="utf-8", newline="\n").write(src)
print("  patched drivers/i2s/i2s_esp32.c")
print("    rx_stop_transfer: stop the I2S RX UNIT before dma_stop() -- the producer, then")
print("                      the consumer, which is the order ESP-IDF's own driver uses")
print("    both:             free the in-flight block ONLY if dma_stop() succeeded")
print("    tx_stop_transfer: unchanged. The TX DMA only reads, and the TX unit is the")
print("                      source of the shared bit clock.")
print("    both:             clear dma_pending, and THEN return the in-flight block")
print("    tx_callback:      NULL mem_block after the free (or the above double-frees)")
print("    rx_callback:      NULL mem_block once the queue owns it (or stop_transfer")
print("                      double-frees it on the graceful-stop path)")
PY
