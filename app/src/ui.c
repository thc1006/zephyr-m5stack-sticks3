/* SPDX-License-Identifier: Apache-2.0 */
#include "ui.h"
#include "gfx.h"
#include "battery.h"

#ifdef CONFIG_APP_BLE
#include "ble.h"
#endif
#ifdef CONFIG_APP_AUDIO
#include "audio.h"
#endif
#ifdef CONFIG_APP_IR
#include "ir.h"
#endif
#ifdef CONFIG_APP_WIFI
#include "wifi_glue.h"
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
#define LINE_GAP   8U         /* vertical gap between body text lines (roomy) */

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
	case PAGE_AUDIO_REC:
		return "REC";
#endif
#ifdef CONFIG_APP_BLE
	case PAGE_BLE:
		return "BLE";
#endif
#ifdef CONFIG_APP_IR
	case PAGE_IR:
		return "IR";
#endif
#ifdef CONFIG_APP_WIFI
	case PAGE_WIFI:
		return "Wi-Fi";
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

	/* State-of-charge from the fuel gauge (issue #8); approximate, voltage-only. */
	if (s->soc_pct >= 0) {
		uint8_t bars = bat_soc_bars(s->soc_pct, 4);

		/* Keep within the 135 px line (~13 chars): no brackets, compact bar. */
		snprintf(line, sizeof(line), "SoC %d%% %.*s%.*s ", s->soc_pct,
			 bars, "####", 4 - bars, "----");
	} else {
		snprintf(line, sizeof(line), "SoC: n/a     ");
	}
	gfx_draw_text(MARGIN_X, body_line_y(1), HOME_FG, HOME_BG, line);

	/* External 5V/USB power vs battery, from the M5PM1 VIN reading. */
	snprintf(line, sizeof(line), "src: %s   ",
		 bat_power_present(s->vin_mv) ? "USB 5V" : "battery");
	gfx_draw_text(MARGIN_X, body_line_y(2), HOME_FG, HOME_BG, line);
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
/*
 * Draw the live mic-level meter "[####]" on body line `row` (folded in from the
 * old standalone AUDIO page). The capture thread updates the level; the UI only
 * READS it and never touches I2S, so redrawing cannot disturb the SPI display
 * (HW-016e). Fixed 6-char width overwrites cleanly on every tick, no clear.
 */
static void draw_mic_meter(uint16_t row)
{
	char m[8];
	uint8_t bars = audio_mic_bars(audio_mic_level());

	m[0] = '[';
	for (uint8_t i = 0; i < 4U; i++) {
		m[1 + i] = (i < bars) ? '#' : '.';
	}
	m[5] = ']';
	m[6] = '\0';
	gfx_draw_text(MARGIN_X, body_line_y(row), HOME_FG, HOME_BG, m);
}

/*
 * PAGE_AUDIO_REC body (issue #14): the modal recorder. Reads audio_rec_get_state()
 * and draws the matching screen; the UI only READS state and NEVER touches I2S
 * (the record/playback runs on the audio thread). Lines are padded to a fixed
 * ~13-char width (the 10px font on the 135px panel) so within-a-mode refreshes
 * overwrite stale digits cleanly; the whole screen is cleared on a mode change
 * (handled in ui_render). English only -- the bundled cfb_font is ASCII.
 */
static void render_audio_rec_body(const struct app_status *s)
{
	char line[20];

	ARG_UNUSED(s);

	/* Every state lays its lines out on consecutive rows (even spacing) and
	 * shows what each button does, so there is always on-screen guidance.
	 */
	switch (audio_rec_get_state()) {
	case AUDIO_REC_RECORDING: {
		uint32_t total = (uint32_t)CONFIG_APP_AUDIO_REC_SECONDS * 1000U;
		uint32_t el = audio_rec_len_ms();
		uint32_t left = (el < total) ? (total - el) : 0U;

		gfx_draw_text(MARGIN_X, body_line_y(0), HOME_FG, HOME_BG, "state: rec   ");
		gfx_draw_text(MARGIN_X, body_line_y(1), HOME_FG, HOME_BG, "SPEAK NOW    ");
		snprintf(line, sizeof(line), "%u.%us left  ",
			 left / 1000U, (left % 1000U) / 100U);
		gfx_draw_text(MARGIN_X, body_line_y(2), HOME_FG, HOME_BG, line);
		gfx_draw_text(MARGIN_X, body_line_y(3), HOME_FG, HOME_BG, "K1 stop      ");
		break;
	}
	case AUDIO_REC_REVIEW:
		gfx_draw_text(MARGIN_X, body_line_y(0), HOME_FG, HOME_BG, "state: review");
		draw_mic_meter(1); /* live mic still shown while reviewing */
		snprintf(line, sizeof(line), "%u.%us pk%-5u",
			 audio_rec_len_ms() / 1000U, (audio_rec_len_ms() % 1000U) / 100U,
			 audio_rec_peak());
		gfx_draw_text(MARGIN_X, body_line_y(2), HOME_FG, HOME_BG, line);
		gfx_draw_text(MARGIN_X, body_line_y(3), HOME_FG, HOME_BG, "K1 play      ");
		gfx_draw_text(MARGIN_X, body_line_y(4), HOME_FG, HOME_BG, "hold K1: rec ");
		gfx_draw_text(MARGIN_X, body_line_y(5), HOME_FG, HOME_BG, "K2 next page ");
		break;
	case AUDIO_REC_PLAYING:
		gfx_draw_text(MARGIN_X, body_line_y(0), HOME_FG, HOME_BG, "state: play  ");
		gfx_draw_text(MARGIN_X, body_line_y(1), HOME_FG, HOME_BG, "playing...   ");
		gfx_draw_text(MARGIN_X, body_line_y(2), HOME_FG, HOME_BG, "(auto-ends)  ");
		gfx_draw_text(MARGIN_X, body_line_y(3), HOME_FG, HOME_BG, "K2 next page ");
		break;
	case AUDIO_REC_IDLE:
	default:
		gfx_draw_text(MARGIN_X, body_line_y(0), HOME_FG, HOME_BG,
			      audio_ready() ? "state: ready " : "init failed  ");
		draw_mic_meter(1); /* live mic level: confirms the device is hearing you */
		gfx_draw_text(MARGIN_X, body_line_y(2), HOME_FG, HOME_BG, "K1 record    ");
		gfx_draw_text(MARGIN_X, body_line_y(3), HOME_FG, HOME_BG, "K2 next page ");
		break;
	}
}
#endif /* CONFIG_APP_AUDIO */

#ifdef CONFIG_APP_IR
static void render_ir_body(const struct app_status *s)
{
	char line[24];
	uint8_t a;
	uint8_t c;

	ARG_UNUSED(s);

	gfx_draw_text(MARGIN_X, body_line_y(0), HOME_FG, HOME_BG,
		      ir_ready() ? "TX ready" : "init failed");
	snprintf(line, sizeof(line), "tx: %u   ", (unsigned int)ir_tx_count());
	gfx_draw_text(MARGIN_X, body_line_y(1), HOME_FG, HOME_BG, line);

	if (ir_rx_last(&a, &c)) {
		snprintf(line, sizeof(line), "NEC %02X:%02X #%u ", a, c,
			 (unsigned int)ir_rx_count());
	} else {
		snprintf(line, sizeof(line), "NEC: --     ");
	}
	gfx_draw_text(MARGIN_X, body_line_y(2), HOME_FG, HOME_BG, line);

	/*
	 * Any remote (any protocol) shows up here as edge activity, even one the
	 * NEC decoder above does not recognise - a generic "IR received" signal.
	 */
	snprintf(line, sizeof(line), "IR act:%u   ", (unsigned int)ir_rx_edges());
	gfx_draw_text(MARGIN_X, body_line_y(3), HOME_FG, HOME_BG, line);
}
#endif /* CONFIG_APP_IR */

#ifdef CONFIG_APP_WIFI
static void render_wifi_body(const struct app_status *s)
{
	struct wifi_ap found[3];
	char line[24];
	size_t n;

	ARG_UNUSED(s);

	switch (wifi_glue_conn_state()) {
	case M5WIFI_STATE_CONNECTED: {
		char ip[16];

		(void)wifi_glue_ipv4(ip, sizeof(ip));
		snprintf(line, sizeof(line), "IP %.15s", ip[0] ? ip : "(dhcp..)");
		break;
	}
	case M5WIFI_STATE_CONNECTING:
		snprintf(line, sizeof(line), "connecting  ");
		break;
	case M5WIFI_STATE_RETRY_WAIT:
		snprintf(line, sizeof(line), "retrying    ");
		break;
	case M5WIFI_STATE_FAILED:
		snprintf(line, sizeof(line), "conn failed ");
		break;
	default: /* IDLE: no connection attempt, show the scan state */
		switch (wifi_glue_state()) {
		case WIFI_GLUE_SCANNING:
			snprintf(line, sizeof(line), "scanning... ");
			break;
		case WIFI_GLUE_DONE:
			snprintf(line, sizeof(line), "scan done   ");
			break;
		case WIFI_GLUE_ERROR:
			snprintf(line, sizeof(line), "scan error  ");
			break;
		case WIFI_GLUE_NO_IFACE:
			snprintf(line, sizeof(line), "no iface    ");
			break;
		default:
			snprintf(line, sizeof(line), "idle        ");
			break;
		}
		break;
	}
	gfx_draw_text(MARGIN_X, body_line_y(0), HOME_FG, HOME_BG, line);

	n = wifi_glue_snapshot(found, ARRAY_SIZE(found));
	for (uint16_t i = 0; i < ARRAY_SIZE(found); i++) {
		if (i < n) {
			snprintf(line, sizeof(line), "%-8.8s %u %.4s ", found[i].ssid,
				 wifi_rssi_bars(found[i].rssi),
				 wifi_sec_str(found[i].security));
		} else {
			snprintf(line, sizeof(line), "            ");
		}
		gfx_draw_text(MARGIN_X, body_line_y((uint16_t)(i + 1)), HOME_FG,
			      HOME_BG, line);
	}
}
#endif /* CONFIG_APP_WIFI */

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
	case PAGE_AUDIO_REC: {
		/* Clear on a page change OR a state change so each screen starts clean
		 * (no residue from a longer previous line); within a state only the
		 * fixed-width dynamic lines (meter, countdown) are redrawn -- no flash.
		 * The live mic meter (folded in from the old AUDIO page) updates here
		 * without a clear because READY/REVIEW keep the same state.
		 */
		static int last_rec_st = -1;
		int st = (int)audio_rec_get_state();

		if (page_changed || st != last_rec_st) {
			gfx_clear(HOME_BG);
			draw_header(page);
			last_rec_st = st;
		}
		render_audio_rec_body(s);
		break;
	}
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
#ifdef CONFIG_APP_IR
	case PAGE_IR:
		if (page_changed) {
			gfx_clear(HOME_BG);
			draw_header(page);
		}
		/* TX is driven from the main loop (periodic) so render stays pure. */
		render_ir_body(s);
		break;
#endif
#ifdef CONFIG_APP_WIFI
	case PAGE_WIFI:
		if (page_changed) {
			gfx_clear(HOME_BG);
			draw_header(page);
		}
		/* The scan is started from the main loop on page entry. */
		render_wifi_body(s);
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
