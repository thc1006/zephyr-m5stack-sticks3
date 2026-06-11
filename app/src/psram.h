/*
 * Copyright (c) 2026 Hsiu-Chi Tsai
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef M5STICKS3_PSRAM_H
#define M5STICKS3_PSRAM_H

/*
 * Octal PSRAM self-test (issue #13). The whole module is gated behind
 * CONFIG_APP_PSRAM (built via overlay-psram.conf); when the option is off the
 * function is not declared and the source is not compiled, so the default build
 * is unchanged and stays PSRAM-free.
 */
#ifdef CONFIG_APP_PSRAM

#include <stdbool.h>

/*
 * One-shot boot self-test of the on-board 8 MB octal SPIRAM
 * (ESP32-S3-PICO-1-N8R8). Allocates a buffer from the external PSRAM region via
 * the shared-multi-heap, writes and reads back a pattern, and confirms the
 * buffer really lands in external RAM (not a silent fall-back to internal SRAM).
 * The result is logged on the console; returns true on pass.
 */
bool psram_selftest(void);

#endif /* CONFIG_APP_PSRAM */

#endif /* M5STICKS3_PSRAM_H */
