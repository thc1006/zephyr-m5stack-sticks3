/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "pages.h"
#include "status.h"
#include "ui.h"
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

/*
 * Wi-Fi and BLE share the ESP32-S3 radio and their coexistence is not a
 * validated Zephyr configuration, so the two stacks must never be compiled
 * together. CONFIG_APP_WIFI depends on !CONFIG_APP_BLE, but the stack Kconfigs
 * are set directly by the overlays, so guard at the stack level too.
 */
BUILD_ASSERT(!(IS_ENABLED(CONFIG_WIFI) && IS_ENABLED(CONFIG_BT)),
	     "Wi-Fi and BLE coexistence is not validated on ESP32-S3; build with "
	     "overlay-wifi.conf OR overlay-ble.conf, not both.");

#define LOOP_MS 1000

#ifdef CONFIG_APP_WIFI
/*
 * Wi-Fi background-scan cadence (ms): the first scan a few seconds after boot,
 * then periodically. A scan stuck in SCANNING past WIFI_SCAN_STUCK_MS is treated
 * as lost so the device keeps scanning instead of freezing.
 */
#define WIFI_SCAN_FIRST_MS  3000
#define WIFI_SCAN_PERIOD_MS 15000
#define WIFI_SCAN_STUCK_MS  30000
#define WIFI_CONNECT_AT_MS  5000 /* auto-connect this long after boot */
#endif

/* KEY1 = G11, KEY2 = G12 (hardware-confirmed; active-low, pull-up). */
#define KEY1_PIN 11
#define KEY2_PIN 12

atomic_t app_current_page = ATOMIC_INIT(PAGE_HOME);

/* Given by the input callback so the main loop re-renders immediately on a
 * button press instead of waiting out its periodic tick.
 */
K_SEM_DEFINE(nav_sem, 0, 1);

void app_page_next(void)
{
	atomic_val_t v = atomic_get(&app_current_page);

	atomic_set(&app_current_page, (v + 1) % PAGE_COUNT);
}

void app_page_prev(void)
{
	atomic_val_t v = atomic_get(&app_current_page);

	atomic_set(&app_current_page, (v + PAGE_COUNT - 1) % PAGE_COUNT);
}

/*
 * gpio-keys reports KEY1/KEY2 via the input subsystem. The callback only
 * advances the atomic page index (KEY1 next, KEY2 prev) on press; all drawing
 * happens in the main loop.
 */
static void input_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY || evt->value == 0) {
		return; /* press only */
	}

	if (evt->code == INPUT_KEY_0) {
		app_page_next();
	} else if (evt->code == INPUT_KEY_1) {
		app_page_prev();
	} else {
		return;
	}

	k_sem_give(&nav_sem); /* wake the loop -> instant redraw */
	printk("BUTTON code=%u -> page=%ld\n", evt->code,
	       (long)atomic_get(&app_current_page));
}
INPUT_CALLBACK_DEFINE(NULL, input_cb, NULL);

int main(void)
{
	printk("M5StickS3 Zephyr validation app\n");
	printk("Board: %s\n", CONFIG_BOARD);

	const struct device *g0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

	status_init();
	ui_init();
#ifdef CONFIG_APP_AUDIO
	(void)audio_init();
#endif
#ifdef CONFIG_APP_BLE
	(void)ble_init();
#endif
#ifdef CONFIG_APP_IR
	(void)ir_init();
#endif
#ifdef CONFIG_APP_WIFI
	(void)wifi_glue_init();
#endif

	while (1) {
		struct app_status st;
		enum app_page page = app_page_get();

		status_sample(&st);
		ui_render(page, &st);
#ifdef CONFIG_APP_BLE
		ble_update(&st);
#endif
#ifdef CONFIG_APP_IR
		/*
		 * Transmit a test NEC frame once on ENTERING the IR page (a loopback
		 * blink, phone-camera visible). Stay quiet afterwards so RX can
		 * receive an external remote cleanly, without TX self-interference.
		 */
		{
			static enum app_page ir_prev = PAGE_COUNT;

			if (page == PAGE_IR && page != ir_prev && ir_ready()) {
				ir_tx_test();
			}
			ir_prev = page;
		}
		{
			static uint32_t ir_rx_seen;
			uint32_t n = ir_rx_count();

			if (n != ir_rx_seen) {
				uint8_t a = 0;
				uint8_t c = 0;

				ir_rx_seen = n;
				(void)ir_rx_last(&a, &c);
				printk("IR RX addr=0x%02x cmd=0x%02x (#%u)\n", a, c,
				       (unsigned int)n);
			}
		}
#endif
#ifdef CONFIG_APP_WIFI
		/*
		 * Start a scan on ENTERING the Wi-Fi page, and otherwise every ~15 s
		 * (first at ~3 s after boot, then periodically) so the AP list stays
		 * fresh and a scan is always present on the serial log. Results arrive
		 * asynchronously and are logged by the glue.
		 */
		{
			static enum app_page wifi_prev = PAGE_COUNT;
			static int64_t wifi_last_scan;
			static bool wifi_conn_tried;

			/*
			 * Auto-connect once, a few seconds after boot, when an SSID is
			 * configured (from the local, untracked credentials overlay). With
			 * no SSID the literal is empty at compile time and this is skipped,
			 * so the default build only scans.
			 */
			if (!wifi_conn_tried && sizeof(CONFIG_APP_WIFI_SSID) > 1 &&
			    st.uptime_ms > WIFI_CONNECT_AT_MS) {
				enum wifi_sec sec = (sizeof(CONFIG_APP_WIFI_PSK) > 1)
							    ? WIFI_SEC_PSK
							    : WIFI_SEC_NONE;

				wifi_conn_tried = true;
				(void)wifi_glue_connect(CONFIG_APP_WIFI_SSID,
							CONFIG_APP_WIFI_PSK, sec);
			}

			if (page == PAGE_WIFI && page != wifi_prev) {
				if (wifi_glue_scan_start() == 0) {
					wifi_last_scan = st.uptime_ms;
				}
			} else if (st.uptime_ms > WIFI_SCAN_FIRST_MS &&
				   wifi_glue_conn_state() != M5WIFI_STATE_CONNECTED &&
				   (wifi_last_scan == 0 ||
				    st.uptime_ms - wifi_last_scan > WIFI_SCAN_PERIOD_MS)) {
				/*
				 * A scan in flight normally blocks a new one, but a scan stuck
				 * in SCANNING past WIFI_SCAN_STUCK_MS is treated as lost (the
				 * esp32 driver can accept the request yet never emit SCAN_DONE
				 * under OOM/RF-busy) so scanning resumes instead of freezing.
				 * Background scanning stops once CONNECTED (scanning while
				 * associated drops frames).
				 */
				bool busy = wifi_glue_state() == WIFI_GLUE_SCANNING;
				int64_t age = st.uptime_ms - wifi_last_scan;

				if (!busy || age > WIFI_SCAN_STUCK_MS) {
					if (wifi_glue_scan_start() == 0) {
						wifi_last_scan = st.uptime_ms;
					}
				}
			}
			wifi_prev = page;
		}
#endif

		int k1 = device_is_ready(g0) ? gpio_pin_get_raw(g0, KEY1_PIN) : -1;
		int k2 = device_is_ready(g0) ? gpio_pin_get_raw(g0, KEY2_PIN) : -1;

		printk("alive uptime_ms=%lld page=%d KEY1=%d KEY2=%d bat=%d soc=%d "
		       "vin=%d imu=%d accel=[%d.%06d %d.%06d %d.%06d]\n",
		       st.uptime_ms, page, k1, k2, st.bat_mv, st.soc_pct, st.vin_mv,
		       st.imu_ok, st.accel[0].val1, abs(st.accel[0].val2),
		       st.accel[1].val1, abs(st.accel[1].val2),
		       st.accel[2].val1, abs(st.accel[2].val2));

		/* Wake early on a button press; otherwise tick every LOOP_MS. */
		k_sem_take(&nav_sem, K_MSEC(LOOP_MS));
	}

	return 0;
}
