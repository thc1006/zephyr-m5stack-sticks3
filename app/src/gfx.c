/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 Hsiu-Chi Tsai */
/*
 * RGB565 graphics layer. See gfx.h for the public API.
 *
 * Byte order: mipi_dbi_spi_write_display() is byte-transparent (it ARG_UNUSEs
 * the pixel format and ships the buffer bytes verbatim to the panel), so the
 * app owns the on-wire byte order. We read display_get_capabilities() once at
 * init: PIXEL_FORMAT_RGB_565 means high-byte-first on the wire (matches the
 * proven P0 fill_screen and the board's default CONFIG_ST7789V_RGB565);
 * PIXEL_FORMAT_RGB_565X means the swapped (low-byte-first) order. Anything
 * else falls back to high-byte-first.
 *
 * Glyph bit-unpacking mirrors Zephyr cfb.c draw_char_vtmono()/get_glyph_byte()
 * for an LSB-first VPACKED font (font1016). See font.h for the (x,y)->bit map.
 */
#include "gfx.h"
#include "font.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(gfx, LOG_LEVEL_INF);

#if DT_NODE_HAS_STATUS(DT_CHOSEN(zephyr_display), okay)
#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
#define LCD_W DT_PROP(DISPLAY_NODE, width)
#define LCD_H DT_PROP(DISPLAY_NODE, height)

static const struct device *const disp = DEVICE_DT_GET(DISPLAY_NODE);
static bool ready;
/* true: write high byte first (RGB_565). false: low byte first (RGB_565X). */
static bool hi_byte_first = true;

/* One-row scratch for fills (LCD_W pixels * 2 bytes). */
static uint8_t line_buf[LCD_W * 2];
/* One-glyph scratch for text (FONT_WIDTH * FONT_HEIGHT pixels * 2 bytes). */
static uint8_t glyph_buf[FONT_WIDTH * FONT_HEIGHT * 2];

/* Store one RGB565 pixel into buf[i*2..] in the detected wire byte order. */
static inline void put_px(uint8_t *buf, size_t i, uint16_t color)
{
	if (hi_byte_first) {
		buf[2 * i] = (uint8_t)(color >> 8);
		buf[2 * i + 1] = (uint8_t)(color & 0xff);
	} else {
		buf[2 * i] = (uint8_t)(color & 0xff);
		buf[2 * i + 1] = (uint8_t)(color >> 8);
	}
}

bool gfx_init(void)
{
	struct display_capabilities caps;

	ready = false;

	if (!device_is_ready(disp)) {
		LOG_WRN("Display not ready");
		return false;
	}

	display_get_capabilities(disp, &caps);

	if (caps.current_pixel_format == PIXEL_FORMAT_RGB_565X) {
		hi_byte_first = false;
		LOG_INF("Pixel format RGB_565X (low byte first)");
	} else {
		/* RGB_565 and any unexpected value: high byte first. */
		hi_byte_first = true;
		if (caps.current_pixel_format != PIXEL_FORMAT_RGB_565) {
			LOG_WRN("Unexpected pixel format %d; assuming RGB_565",
				caps.current_pixel_format);
		} else {
			LOG_INF("Pixel format RGB_565 (high byte first)");
		}
	}

	/* Request RGB565 if the driver allows it; ignore -ENOTSUP otherwise. */
	if (caps.current_pixel_format != PIXEL_FORMAT_RGB_565) {
		(void)display_set_pixel_format(disp, PIXEL_FORMAT_RGB_565);
	}

	display_blanking_off(disp);
	LOG_INF("Display ready (%dx%d)", LCD_W, LCD_H);

	ready = true;
	return true;
}

bool gfx_ready(void)
{
	return ready;
}

uint16_t gfx_width(void)
{
	return LCD_W;
}

uint16_t gfx_height(void)
{
	return LCD_H;
}

void gfx_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
	struct display_buffer_descriptor desc;

	if (!ready || w == 0U || h == 0U || x >= LCD_W || y >= LCD_H) {
		return;
	}

	/* Clip to panel. */
	if ((uint32_t)x + w > LCD_W) {
		w = (uint16_t)(LCD_W - x);
	}
	if ((uint32_t)y + h > LCD_H) {
		h = (uint16_t)(LCD_H - y);
	}

	for (uint16_t i = 0; i < w; i++) {
		put_px(line_buf, i, color);
	}

	desc.buf_size = (uint32_t)w * 2U;
	desc.width = w;
	desc.height = 1U;
	desc.pitch = w;

	for (uint16_t row = 0; row < h; row++) {
		if (display_write(disp, x, y + row, &desc, line_buf) != 0) {
			break;
		}
	}
}

void gfx_clear(uint16_t color)
{
	gfx_fill_rect(0, 0, LCD_W, LCD_H, color);
}

uint16_t gfx_font_w(void)
{
	return FONT_WIDTH;
}

uint16_t gfx_font_h(void)
{
	return FONT_HEIGHT;
}

void gfx_draw_char(uint16_t x, uint16_t y, uint16_t fg, uint16_t bg, char c)
{
	struct display_buffer_descriptor desc;
	const uint8_t *glyph;
	uint8_t uc = (uint8_t)c;

	if (!ready || x >= LCD_W || y >= LCD_H) {
		return;
	}

	/* Map unsupported characters to space so layout stays aligned. */
	if (uc < FONT_FIRST_CHAR || uc > FONT_LAST_CHAR) {
		uc = (uint8_t)' ';
	}
	glyph = font1016_glyphs[uc - FONT_FIRST_CHAR];

	/*
	 * Expand the column-major, LSB-first VPACKED glyph into a row-major
	 * RGB565 block. Pixel (gx, gy) is set iff
	 *   glyph[gx * FONT_BYTES_PER_COL + (gy / 8)] & BIT(gy % 8).
	 */
	for (uint16_t gy = 0; gy < FONT_HEIGHT; gy++) {
		for (uint16_t gx = 0; gx < FONT_WIDTH; gx++) {
			uint8_t byte = glyph[gx * FONT_BYTES_PER_COL + (gy / 8U)];
			bool on = (byte & BIT(gy % 8U)) != 0U;
			size_t idx = (size_t)gy * FONT_WIDTH + gx;

			put_px(glyph_buf, idx, on ? fg : bg);
		}
	}

	/* Clip width/height if the glyph would run off the panel edge. */
	uint16_t w = FONT_WIDTH;
	uint16_t h = FONT_HEIGHT;

	if ((uint32_t)x + w > LCD_W) {
		w = (uint16_t)(LCD_W - x);
	}
	if ((uint32_t)y + h > LCD_H) {
		h = (uint16_t)(LCD_H - y);
	}

	desc.buf_size = (uint32_t)FONT_WIDTH * h * 2U;
	desc.width = w;
	desc.height = h;
	desc.pitch = FONT_WIDTH; /* glyph_buf rows are always FONT_WIDTH wide */

	(void)display_write(disp, x, y, &desc, glyph_buf);
}

void gfx_draw_text(uint16_t x, uint16_t y, uint16_t fg, uint16_t bg, const char *s)
{
	uint16_t cx = x;

	if (!ready || s == NULL) {
		return;
	}

	for (; *s != '\0'; s++) {
		if (cx >= LCD_W) {
			break;
		}
		gfx_draw_char(cx, y, fg, bg, *s);
		cx = (uint16_t)(cx + FONT_WIDTH);
	}
}

#else /* no display chosen */

bool gfx_init(void) { return false; }
bool gfx_ready(void) { return false; }
uint16_t gfx_width(void) { return 0; }
uint16_t gfx_height(void) { return 0; }
uint16_t gfx_font_w(void) { return FONT_WIDTH; }
uint16_t gfx_font_h(void) { return FONT_HEIGHT; }
void gfx_clear(uint16_t color) { ARG_UNUSED(color); }
void gfx_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
	ARG_UNUSED(x); ARG_UNUSED(y); ARG_UNUSED(w); ARG_UNUSED(h); ARG_UNUSED(color);
}
void gfx_draw_char(uint16_t x, uint16_t y, uint16_t fg, uint16_t bg, char c)
{
	ARG_UNUSED(x); ARG_UNUSED(y); ARG_UNUSED(fg); ARG_UNUSED(bg); ARG_UNUSED(c);
}
void gfx_draw_text(uint16_t x, uint16_t y, uint16_t fg, uint16_t bg, const char *s)
{
	ARG_UNUSED(x); ARG_UNUSED(y); ARG_UNUSED(fg); ARG_UNUSED(bg); ARG_UNUSED(s);
}

#endif
