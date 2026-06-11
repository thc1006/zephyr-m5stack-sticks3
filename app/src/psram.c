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

	/* Write a position-dependent pattern, then read it back. A position
	 * dependence catches stuck address lines that a constant fill would miss.
	 */
	for (uint32_t i = 0; i < PSRAM_TEST_BYTES; i++) {
		buf[i] = (uint8_t)(i ^ 0xA5U);
	}

	int errors = 0;

	for (uint32_t i = 0; i < PSRAM_TEST_BYTES; i++) {
		if (buf[i] != (uint8_t)(i ^ 0xA5U)) {
			errors++;
		}
	}

	shared_multi_heap_free(buf);

	if (errors != 0) {
		LOG_ERR("%d byte mismatch(es) in %u B external R/W", errors,
			PSRAM_TEST_BYTES);
		return false;
	}

	LOG_INF("octal SPIRAM mapped; %u B external R/W verified OK",
		PSRAM_TEST_BYTES);
	return true;
}

#endif /* CONFIG_APP_PSRAM */
