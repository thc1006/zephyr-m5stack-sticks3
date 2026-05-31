/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 Hsiu-Chi Tsai */
/*
 * Minimal RGB565 graphics layer for the StickS3 LCD (sitronix,st7789v over
 * mipi-dbi-spi, 135x240, RGB565). Wraps the Zephyr display driver and renders
 * monochrome CFB glyphs (see font.{c,h}) directly as RGB565 pixels using a
 * small reusable line buffer -- no full framebuffer is allocated.
 */
#ifndef M5STICKS3_GFX_H
#define M5STICKS3_GFX_H

#include <stdbool.h>
#include <stdint.h>

/* Convenience RGB565 colours (logical value; gfx handles wire byte order). */
#define GFX_BLACK 0x0000U
#define GFX_WHITE 0xFFFFU
#define GFX_RED   0xF800U
#define GFX_GREEN 0x07E0U
#define GFX_BLUE  0x001FU

/*
 * Bring up the chosen display: DEVICE_DT_GET + device_is_ready, query
 * display_get_capabilities to lock the RGB565 wire byte order, request
 * PIXEL_FORMAT_RGB_565 if supported, and disable blanking.
 * Returns true on success (display ready and usable), false otherwise.
 */
bool gfx_init(void);

/* True once gfx_init() has succeeded. */
bool gfx_ready(void);

/* Panel dimensions (from devicetree), valid after gfx_init(). */
uint16_t gfx_width(void);
uint16_t gfx_height(void);

/* Font metrics (the vendored CFB font). */
uint16_t gfx_font_w(void);
uint16_t gfx_font_h(void);

/* Fill the whole panel with one RGB565 colour. */
void gfx_clear(uint16_t color);

/* Fill an axis-aligned rectangle; clipped to the panel bounds. */
void gfx_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/* Draw a single glyph at (x, y) with the given fg/bg RGB565 colours. */
void gfx_draw_char(uint16_t x, uint16_t y, uint16_t fg, uint16_t bg, char c);

/* Draw a NUL-terminated string left-to-right starting at (x, y). */
void gfx_draw_text(uint16_t x, uint16_t y, uint16_t fg, uint16_t bg, const char *s);

#endif /* M5STICKS3_GFX_H */
