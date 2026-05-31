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

#define LOOP_MS 1000

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

	while (1) {
		struct app_status st;
		enum app_page page = app_page_get();

		status_sample(&st);
		ui_render(page, &st);
#ifdef CONFIG_APP_BLE
		ble_update(&st);
#endif

		int k1 = device_is_ready(g0) ? gpio_pin_get_raw(g0, KEY1_PIN) : -1;
		int k2 = device_is_ready(g0) ? gpio_pin_get_raw(g0, KEY2_PIN) : -1;

		printk("alive uptime_ms=%lld page=%d KEY1=%d KEY2=%d imu=%d "
		       "accel=[%d.%06d %d.%06d %d.%06d]\n",
		       st.uptime_ms, page, k1, k2, st.imu_ok,
		       st.accel[0].val1, abs(st.accel[0].val2),
		       st.accel[1].val1, abs(st.accel[1].val2),
		       st.accel[2].val1, abs(st.accel[2].val2));

		/* Wake early on a button press; otherwise tick every LOOP_MS. */
		k_sem_take(&nav_sem, K_MSEC(LOOP_MS));
	}

	return 0;
}
