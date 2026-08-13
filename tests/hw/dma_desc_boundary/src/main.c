/*
 * Copyright (c) 2026 Hsiu-Chi Tsai
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Descriptor-exhaustion boundary check for the ESP32 GDMA driver, covering
 * zephyrproject-rtos/zephyr#115502 and the fix in PR #115507.
 *
 * dma_esp32_config_descriptor() fills at most CONFIG_DMA_ESP32_MAX_DESCRIPTOR_NUM
 * descriptors, each carrying at most DMA_DESCRIPTOR_BUFFER_MAX_SIZE_4B_ALIGNED
 * (4095 - 3 = 4092) bytes, so with the default 16 descriptors:
 *
 *   N     = 16 * 4092 = 65472  fits exactly, and must be accepted
 *   N + 1 =             65473  needs a 17th descriptor, and must be refused
 *
 * N + 1 is the input that leaves the walking pointer one past the end of
 * desc_list. Before the fix the exhaustion test dereferenced that pointer, so
 * the answer came from whatever follows the array; after it the pointer is
 * compared against the end of the array instead.
 *
 * Only dma_config() runs here. The transfer is never started, so nothing is
 * read or written through the descriptors. What this exercises is the
 * descriptor-list construction and its exhaustion check, which is the code the
 * fix changes.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/sys/printk.h>
#include <errno.h>

#define DESC_MAX      CONFIG_DMA_ESP32_MAX_DESCRIPTOR_NUM
#define DESC_BYTES    4092U
#define SIZE_FITS     ((uint32_t)DESC_MAX * DESC_BYTES)
#define SIZE_OVERFLOW (SIZE_FITS + 1U)

/*
 * Only the start address is checked for DMA capability, and the transfer is
 * never started, so a small internal-SRAM buffer is enough to drive the
 * descriptor count.
 */
static uint8_t dma_buf[4096] __aligned(4);

static int try_size(const struct device *dma, uint32_t channel, uint32_t size)
{
	struct dma_block_config blk = {0};
	struct dma_config cfg = {0};

	blk.source_address = (uint32_t)dma_buf;
	blk.dest_address = (uint32_t)dma_buf;
	blk.block_size = size;
	blk.next_block = NULL;

	cfg.channel_direction = MEMORY_TO_MEMORY;
	cfg.source_data_size = 4;
	cfg.dest_data_size = 4;
	cfg.source_burst_length = 4;
	cfg.dest_burst_length = 4;
	cfg.block_count = 1;
	cfg.head_block = &blk;

	return dma_config(dma, channel, &cfg);
}

int main(void)
{
	const struct device *dma = DEVICE_DT_GET(DT_NODELABEL(dma));
	int fits;
	int over;

	printk("\n=== esp32 gdma descriptor boundary (#115502 / PR #115507) ===\n");

	if (!device_is_ready(dma)) {
		printk("RESULT: FAIL (dma device not ready)\n");
		return 0;
	}

	printk("descriptors      = %d\n", DESC_MAX);
	printk("bytes/descriptor = %u\n", DESC_BYTES);
	printk("N                = %u bytes\n", SIZE_FITS);
	printk("N+1              = %u bytes\n", SIZE_OVERFLOW);

	fits = try_size(dma, 0, SIZE_FITS);
	over = try_size(dma, 0, SIZE_OVERFLOW);

	printk("dma_config(N)    = %d  (expect 0)\n", fits);
	printk("dma_config(N+1)  = %d  (expect %d, -EINVAL)\n", over, -EINVAL);
	printk("RESULT: %s\n",
	       (fits == 0 && over == -EINVAL) ? "PASS" : "FAIL");

	return 0;
}
