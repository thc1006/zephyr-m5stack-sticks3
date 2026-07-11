/*
 * Copyright (c) 2026 Hsiu-Chi Tsai
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Octal PSRAM bring-up self-test for the M5StickS3 (issue #13).
 *
 * The board is an ESP32-S3-PICO-1-N8R8: 8 MB flash + 8 MB *octal* SPIRAM. The
 * default build leaves the PSRAM off (the validation app does not need it and it
 * breaks Wi-Fi on this silicon); overlay-psram.conf turns it on via
 * CONFIG_ESP_SPIRAM / CONFIG_SPIRAM_MODE_OCT and compiles this module.
 *
 * The PSRAM is registered as a shared-multi-heap region with the EXTERNAL
 * attribute. The self-test allocates from that region, exercises it with a
 * pattern, and confirms (via esp_ptr_external_ram) that the buffer really lands
 * in external RAM rather than silently falling back to internal SRAM. Modeled on
 * zephyr/samples/boards/espressif/spiram_test.
 */

#include "psram.h"

#ifdef CONFIG_APP_PSRAM

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/multi_heap/shared_multi_heap.h>
#include <soc/soc_memory_layout.h>

LOG_MODULE_REGISTER(psram, LOG_LEVEL_INF);

/* Test buffer: 64 KB, comfortably larger than any single internal-SRAM block,
 * so a fall-back to internal RAM would be obvious (and is caught explicitly by
 * the esp_ptr_external_ram() check below regardless of size).
 */
#define PSRAM_TEST_BYTES (64U * 1024U)

bool psram_selftest(void)
{
	uint8_t *buf = shared_multi_heap_aligned_alloc(SMH_REG_ATTR_EXTERNAL, 32,
						       PSRAM_TEST_BYTES);
	if (buf == NULL) {
		LOG_ERR("external alloc of %u B failed (SPIRAM not mapped?)",
			PSRAM_TEST_BYTES);
		return false;
	}

	if (!esp_ptr_external_ram(buf)) {
		LOG_ERR("buffer %p is not in external RAM", (void *)buf);
		shared_multi_heap_free(buf);
		return false;
	}

	/*
	 * A 32-bit word carrying the whole word index, not a byte carrying eight
	 * bits of it.
	 *
	 * The previous pattern was `buf[i] = (uint8_t)(i ^ 0xA5)`, and its comment
	 * claimed it "catches stuck address lines". It does not. Truncating to a
	 * byte makes the pattern repeat every 256 bytes, so if A8 were stuck the
	 * cells at 0x0000 and 0x0100 would alias, both would be written 0xA5, both
	 * would read back 0xA5, and both would be *expected* to be 0xA5. The test
	 * passes with the fault present. Only A0 to A7 were ever covered, and the
	 * comment asserted otherwise for a month.
	 *
	 * Every word now carries its own index, so two aliased addresses hold
	 * different expected values and the second write destroys the first. The
	 * second pass writes the complement, which catches a cell stuck at whatever
	 * the first pass happened to leave in it.
	 */
	uint32_t *words = (uint32_t *)buf;
	const uint32_t nwords = PSRAM_TEST_BYTES / sizeof(uint32_t);
	int errors = 0;

	for (uint32_t i = 0; i < nwords; i++) {
		words[i] = i ^ 0xA5A5A5A5U;
	}
	for (uint32_t i = 0; i < nwords; i++) {
		if (words[i] != (i ^ 0xA5A5A5A5U)) {
			errors++;
		}
	}

	for (uint32_t i = 0; i < nwords; i++) {
		words[i] = ~(i ^ 0xA5A5A5A5U);
	}
	for (uint32_t i = 0; i < nwords; i++) {
		if (words[i] != ~(i ^ 0xA5A5A5A5U)) {
			errors++;
		}
	}

	/*
	 * Address aliasing, checked directly rather than hoped for. Write a unique
	 * marker at every power-of-two word offset, then read them all back: if any
	 * address bit is dead, two of these markers land in the same cell and the
	 * earlier one is gone. This is the check the old comment was claiming.
	 */
	for (uint32_t bit = 1U; bit < nwords; bit <<= 1) {
		words[bit] = 0xDEAD0000U | bit;
	}
	words[0] = 0xDEAD0000U;

	for (uint32_t bit = 1U; bit < nwords; bit <<= 1) {
		if (words[bit] != (0xDEAD0000U | bit)) {
			LOG_ERR("address alias: word offset %u reads 0x%08x, expected 0x%08x",
				bit, words[bit], 0xDEAD0000U | bit);
			errors++;
		}
	}
	if (words[0] != 0xDEAD0000U) {
		LOG_ERR("address alias: word offset 0 was overwritten (0x%08x)", words[0]);
		errors++;
	}

	shared_multi_heap_free(buf);

	if (errors != 0) {
		LOG_ERR("%d mismatch(es) in the %u B external R/W test", errors,
			PSRAM_TEST_BYTES);
		return false;
	}

	/*
	 * Say exactly what was proven. 64 KiB is 0.78 percent of the 8 MiB the ROM
	 * reports, and this run does not touch the rest of it.
	 */
	LOG_INF("octal SPIRAM mapped; %u B of the external heap verified "
		"(pattern, complement, address alias)", PSRAM_TEST_BYTES);
	return true;
}

#endif /* CONFIG_APP_PSRAM */
