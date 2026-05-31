/* SPDX-License-Identifier: Apache-2.0 */
#include "ui.h"
#include "gfx.h"

#ifdef CONFIG_APP_BLE
#include "ble.h"
#endif
#ifdef CONFIG_APP_AUDIO
#include "audio.h"
#endif

#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ui, LOG_LEVEL_INF);

#define HOME_BG    GFX_BLACK  /* dark background */
#define HOME_FG    GFX_WHITE  /* white text */

/* Header bar (top of the info pages): white background, black text. */
#define HDR_BG     GFX_WHITE
#define HDR_FG     GFX_BLACK

#define MARGIN_X   4U         /* left text margin */
#define LINE_GAP   4U         /* vertical gap between body text lines */

#if defined(CONFIG_APP_DISPLAY_DEMO) && \
	DT_NODE_HAS_STATUS(DT_CHOSEN(zephyr_display), okay)
static const struct device *const backlight =
	DEVICE_DT_GET(DT_NODELABEL(lcd_backlight));
#endif

/*
 * Last page actually rendered. PAGE_COUNT is used as the "nothing rendered
 * yet" sentinel so the very first ui_render() takes the page-change path
 * (full clear + header + body). On a periodic refresh of the same page we
 * redraw only the dynamic body lines, so there is no 1 Hz full-screen flash.
 */
static enum app_page last_page = PAGE_COUNT;

static const char *page_name(enum app_page page)
{
	switch (page) {
	case PAGE_HOME:
		return "HOME";
	case PAGE_IMU:
		return "IMU";
	case PAGE_POWER:
		return "POWER";
#ifdef CONFIG_APP_AUDIO
	case PAGE_AUDIO:
		return "AUDIO";
#endif
#ifdef CONFIG_APP_BLE
	case PAGE_BLE:
		return "BLE";
#endif
	case PAGE_DIAG:
		return "DIAG";
	default:
		return "?";
	}
}

/* Top of the body text, just below the header bar. */
static inline uint16_t body_top(void)
{
	return (uint16_t)(gfx_font_h() + LINE_GAP);
}

/* Y coordinate of body line n (0-based), below the header. */
static inline uint16_t body_line_y(uint16_t n)
{
	return (uint16_t)(body_top() + n * (gfx_font_h() + LINE_GAP));
}

/* Draw the full-width inverted header bar with the page name at y=0. */
static void draw_header(enum app_page page)
{
	gfx_fill_rect(0, 0, gfx_width(), gfx_font_h(), HDR_BG);
	gfx_draw_text(MARGIN_X, 0, HDR_FG, HDR_BG, page_name(page));
}

/*
 * Dynamic body lines for an info page. Called on every render of that page;
 * each line uses a fixed-width format and gfx_draw_text writes the background
 * behind every glyph, so same-position text overwrites the previous value
 * cleanly without a clear (no residue, no flicker).
 */
static void render_home_body(const struct app_status *s)
{
	char line[24];
	uint32_t secs = (uint32_t)(s->uptime_ms / 1000);

	gfx_draw_text(MARGIN_X, body_line_y(0), HOME_FG, HOME_BG, "M5StickS3");

	snprintf(line, sizeof(line), "up: %us   ", secs);
	gfx_draw_text(MARGIN_X, body_line_y(1), HOME_FG, HOME_BG, line);

	/* Battery voltage from the M5PM1 ADC (P3); -1 = not available. */
	if (s->bat_mv >= 0) {
		snprintf(line, sizeof(line), "bat: %d mV   ", s->bat_mv);
	} else {
		snprintf(line, sizeof(line), "bat: n/a   ");
	}
	gfx_draw_text(MARGIN_X, body_line_y(2), HOME_FG, HOME_BG, line);
}

static void render_imu_body(const struct app_status *s)
{
	static const char axis[3] = {'X', 'Y', 'Z'};

	for (int i = 0; i < 3; i++) {
		char line[24]; /* fixed-width %02u.%02u; 24 avoids -Wformat-truncation */
		int32_t v1 = s->accel[i].val1;
		int32_t v2 = s->accel[i].val2;
		bool neg = (v1 < 0) || (v2 < 0);
		unsigned int whole = (unsigned int)(v1 < 0 ? -v1 : v1);
		unsigned int frac =
			(unsigned int)((v2 < 0 ? -(int64_t)v2 : (int64_t)v2) / 10000);

		/*
		 * m/s^2 with 2 decimals. A sensor_value with magnitude < 1
		 * carries its sign only in val2, so derive one sign for the
		 * whole reading and print magnitudes. Fixed width keeps every
		 * line the same length, so the bg behind each glyph clears the
		 * prior value (no residue, no clear needed).
		 */
		snprintf(line, sizeof(line), "%c:%c%02u.%02u", axis[i],
			 neg ? '-' : '+', whole, frac);
		gfx_draw_text(MARGIN_X, body_line_y((uint16_t)i),
			      HOME_FG, HOME_BG, line);
	}
}

static void render_power_body(const struct app_status *s)
{
	char line[24];

	/* Real VBAT from the M5PM1 ADC (P3); -1 = read failed / unavailable. */
	if (s->bat_mv >= 0) {
		snprintf(line, sizeof(line), "VBAT: %d mV   ", s->bat_mv);
	} else {
		snprintf(line, sizeof(line), "VBAT: n/a    ");
	}
	gfx_draw_text(MARGIN_X, body_line_y(0), HOME_FG, HOME_BG, line);
	gfx_draw_text(MARGIN_X, body_line_y(1), HOME_FG, HOME_BG, "M5PM1 ch1");
}

#ifdef CONFIG_APP_BLE
static void render_ble_body(const struct app_status *s)
{
	char line[24];

	ARG_UNUSED(s);

	snprintf(line, sizeof(line), "adv: %s   ",
		 ble_is_advertising() ? "on" : "off");
	gfx_draw_text(MARGIN_X, body_line_y(0), HOME_FG, HOME_BG, line);

	snprintf(line, sizeof(line), "conn: %u   ",
		 (unsigned int)ble_conn_count());
	gfx_draw_text(MARGIN_X, body_line_y(1), HOME_FG, HOME_BG, line);

	gfx_draw_text(MARGIN_X, body_line_y(2), HOME_FG, HOME_BG,
		      CONFIG_BT_DEVICE_NAME);
}
#endif /* CONFIG_APP_BLE */

#ifdef CONFIG_APP_AUDIO
static void render_audio_body(const struct app_status *s)
{
	ARG_UNUSED(s);

	gfx_draw_text(MARGIN_X, body_line_y(0), HOME_FG, HOME_BG, "440 Hz beep");
	gfx_draw_text(MARGIN_X, body_line_y(1), HOME_FG, HOME_BG,
		      audio_ready() ? "enter=beep" : "init failed");
}
#endif /* CONFIG_APP_AUDIO */

void ui_init(void)
{
	if (gfx_init()) {
		/* Stable blue first frame, like P0, so a blank panel is obvious. */
		gfx_clear(GFX_BLUE);
	}
	last_page = PAGE_COUNT; /* force a full draw on the first render */
}

/*
 * P2 page rendering.
 *
 * Flicker avoidance: the whole screen is cleared only when the page changes
 * (page != last_page). On that path we clear, draw the header, then draw the
 * full body. On a periodic refresh of the *same* info page we redraw only the
 * dynamic body lines -- no gfx_clear -- so the 1 Hz tick no longer flashes the
 * panel. PAGE_DIAG is animated and intentionally re-renders every tick (its
 * colour-cycle + backlight-blink behaviour is unchanged from P1).
 */
void ui_render(enum app_page page, const struct app_status *s)
{
	bool page_changed = (page != last_page);

	if (!gfx_ready()) {
		return;
	}

	switch (page) {
	case PAGE_HOME:
		if (page_changed) {
			gfx_clear(HOME_BG);
			draw_header(page);
		}
		render_home_body(s);
		break;
	case PAGE_IMU:
		if (page_changed) {
			gfx_clear(HOME_BG);
			draw_header(page);
		}
		render_imu_body(s);
		break;
	case PAGE_POWER:
		if (page_changed) {
			gfx_clear(HOME_BG);
			draw_header(page);
		}
		render_power_body(s);
		break;
#ifdef CONFIG_APP_AUDIO
	case PAGE_AUDIO:
		if (page_changed) {
			gfx_clear(HOME_BG);
			draw_header(page);
			render_audio_body(s);
			/*
			 * Beep on ENTER only (page-change edge), so the nav keys
			 * KEY1/KEY2 are not hijacked. The beep is blocking
			 * (~250 ms incl. amp settle/drain); it enables the amp
			 * only for that window.
			 */
			if (audio_ready()) {
				audio_beep();
			}
		} else {
			render_audio_body(s);
		}
		break;
#endif
#ifdef CONFIG_APP_BLE
	case PAGE_BLE:
		if (page_changed) {
			gfx_clear(HOME_BG);
			draw_header(page);
		}
		render_ble_body(s);
		break;
#endif
	case PAGE_DIAG:
#if defined(CONFIG_APP_DISPLAY_DEMO) && \
	DT_NODE_HAS_STATUS(DT_CHOSEN(zephyr_display), okay)
	{
		static const uint16_t colors[] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
		static int ci;
		static uint32_t tick;

		gfx_clear(colors[ci]);
		ci = (ci + 1) % (int)ARRAY_SIZE(colors);

		/* Every ~5 renders blink the backlight (relocated B-3 demo;
		 * markers + timing kept identical so docs/evidence stay valid).
		 */
		if (device_is_ready(backlight) && (++tick % 5U) == 0U) {
			printk(">>> Backlight OFF (regulator_disable)\n");
			(void)regulator_disable(backlight);
			k_msleep(1500);
			(void)regulator_enable(backlight);
			printk(">>> Backlight ON (regulator_enable)\n");
		}
	}
#else
		gfx_clear(GFX_BLACK);
#endif
		break;
	default:
		gfx_clear(GFX_BLACK);
		break;
	}

	last_page = page;
}
