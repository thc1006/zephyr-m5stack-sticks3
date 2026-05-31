/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 Hsiu-Chi Tsai */
#ifndef M5STICKS3_BLE_H
#define M5STICKS3_BLE_H

#include <stdbool.h>
#include <stdint.h>

#include "status.h"

/*
 * Optional BLE telemetry, gated behind CONFIG_APP_BLE. The default build does
 * not enable CONFIG_BT, so these become no-ops (and ble.c is not even compiled,
 * see CMakeLists.txt). Callers in main.c guard the calls with #ifdef CONFIG_APP_BLE
 * so the non-BLE build is byte-identical.
 */

/*
 * Bring up the controller (bt_enable), register the GATT telemetry service, and
 * start connectable + scannable legacy advertising. Returns 0 on success or a
 * negative errno on failure (advertising/GATT then simply stay inactive).
 */
int ble_init(void);

/*
 * Refresh the advertised manufacturer data and the GATT characteristic value
 * from the latest status, and notify subscribed clients. Internally throttled
 * to ~1 Hz, so it is safe to call once per main-loop tick.
 */
void ble_update(const struct app_status *s);

/* True once advertising has been started successfully. */
bool ble_is_advertising(void);

/* Number of currently active central connections. */
uint32_t ble_conn_count(void);

#endif /* M5STICKS3_BLE_H */
