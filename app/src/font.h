/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 Hsiu-Chi Tsai */
/*
 * Glyph data vendored from Zephyr subsys/fb/cfb_fonts.c (Apache-2.0).
 *
 * font1016 is the 10x16 DroidSansMono-derived CFB font. Its CFB caps are
 * CFB_FONT_MONO_VPACKED (and *not* CFB_FONT_MSB_FIRST, i.e. LSB-first).
 *
 * Packing reference (see app/src/gfx.c gfx_draw_char):
 *   - Column-major: FONT_BYTES_PER_COL == height / 8 bytes per x-column.
 *   - Glyph byte for (x, y): data[x * FONT_BYTES_PER_COL + (y / 8)].
 *   - Pixel (x, y) is set iff that byte has bit (y % 8) set (LSB-first:
 *     bit 0 = topmost pixel of the 8-pixel vertical tile).
 * This mirrors cfb.c get_glyph_byte()/draw_char_vtmono() for an LSB-first
 * VPACKED font drawn into an LSB-first vtiled buffer (no bit reversal).
 */
#ifndef M5STICKS3_FONT_H
#define M5STICKS3_FONT_H

#include <stdint.h>

#define FONT_WIDTH        10U
#define FONT_HEIGHT       16U
#define FONT_FIRST_CHAR   32U
#define FONT_LAST_CHAR    126U
#define FONT_BYTES_PER_COL (FONT_HEIGHT / 8U)             /* 2 */
#define FONT_BYTES_PER_GLYPH (FONT_WIDTH * FONT_BYTES_PER_COL) /* 20 */
#define FONT_NUM_GLYPHS   (FONT_LAST_CHAR - FONT_FIRST_CHAR + 1U) /* 95 */

/*
 * Column-major monochrome glyph table, LSB-first VPACKED (see header comment).
 * Indexed [glyph][FONT_BYTES_PER_GLYPH], glyph = char - FONT_FIRST_CHAR.
 */
extern const uint8_t font1016_glyphs[FONT_NUM_GLYPHS][FONT_BYTES_PER_GLYPH];

#endif /* M5STICKS3_FONT_H */
