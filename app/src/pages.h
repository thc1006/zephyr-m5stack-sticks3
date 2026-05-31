/* SPDX-License-Identifier: Apache-2.0 */
#ifndef M5STICKS3_PAGES_H
#define M5STICKS3_PAGES_H

#include <zephyr/sys/atomic.h>

/*
 * Demo pages. Pages for optional features only exist when their Kconfig is
 * enabled, so navigation never lands on an empty screen. The page index is
 * stored in an atomic and advanced by the input callback (no drawing there).
 *
 * PAGE_BLE only exists when CONFIG_APP_BLE is set, and PAGE_AUDIO only when
 * CONFIG_APP_AUDIO is set, so the default build keeps the original
 * HOME/IMU/POWER/DIAG ordering and PAGE_COUNT value.
 */
enum app_page {
	PAGE_HOME = 0,
	PAGE_IMU,
	PAGE_POWER,
#ifdef CONFIG_APP_AUDIO
	PAGE_AUDIO,
#endif
#ifdef CONFIG_APP_BLE
	PAGE_BLE,
#endif
#ifdef CONFIG_APP_IR
	PAGE_IR,
#endif
	PAGE_DIAG,
	PAGE_COUNT,
};

extern atomic_t app_current_page;

static inline enum app_page app_page_get(void)
{
	return (enum app_page)atomic_get(&app_current_page);
}

void app_page_next(void);
void app_page_prev(void);

#endif /* M5STICKS3_PAGES_H */
