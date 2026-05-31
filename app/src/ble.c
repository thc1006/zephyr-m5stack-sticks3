/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 Hsiu-Chi Tsai */
/*
 * Optional BLE telemetry for the M5StickS3 validation app.
 *
 * Entirely gated behind CONFIG_APP_BLE: this translation unit is only compiled
 * when CONFIG_APP_BLE=y (see CMakeLists.txt), and every line below is also under
 * #ifdef CONFIG_APP_BLE so an accidental compile without the option is a no-op.
 *
 * What it does:
 *   - Defines a custom 128-bit primary GATT service with one telemetry
 *     characteristic (READ + NOTIFY) plus its CCC descriptor.
 *   - Starts connectable + scannable legacy advertising whose advertising data
 *     carries flags, the device name, and manufacturer-specific data holding the
 *     same packed telemetry payload (so a scanner sees live data without
 *     connecting, and a GATT client can read/subscribe for notifications).
 *   - Tracks connect/disconnect for the UI.
 *
 * Telemetry payload (struct ble_telemetry, packed, little-endian on the wire):
 *   uint32_t uptime_s     seconds since boot (uptime_ms / 1000)
 *   int16_t  bat_mv       battery millivolts, or -1 when unavailable
 *   int16_t  accel_mg[3]  X/Y/Z acceleration in milli-g (1 g == 1000)
 *
 * Acceleration scaling: the IMU reports m/s^2 as sensor_value{val1,val2}. We
 * convert to milli-g with 1 g = 9.80665 m/s^2, i.e.
 *   mg = (m/s^2) * 1000 / 9.80665
 * clamped to int16 range. milli-g was chosen (over raw m/s^2*100) because it is
 * the unit consumers expect from accelerometers and keeps a full +/-16 g span
 * inside int16.
 */

#ifdef CONFIG_APP_BLE

#include "ble.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

LOG_MODULE_REGISTER(ble, LOG_LEVEL_INF);

/*
 * Custom 128-bit UUIDs (randomly generated; project-local, not registered).
 *   Service:        a3b1f000-5e3a-4c7d-9f2a-7b1c5e3a4c7d
 *   Telemetry char: a3b1f001-5e3a-4c7d-9f2a-7b1c5e3a4c7d
 */
#define BT_UUID_M5_TELEM_SVC_VAL \
	BT_UUID_128_ENCODE(0xa3b1f000, 0x5e3a, 0x4c7d, 0x9f2a, 0x7b1c5e3a4c7d)
#define BT_UUID_M5_TELEM_CHR_VAL \
	BT_UUID_128_ENCODE(0xa3b1f001, 0x5e3a, 0x4c7d, 0x9f2a, 0x7b1c5e3a4c7d)

static const struct bt_uuid_128 telem_svc_uuid =
	BT_UUID_INIT_128(BT_UUID_M5_TELEM_SVC_VAL);
static const struct bt_uuid_128 telem_chr_uuid =
	BT_UUID_INIT_128(BT_UUID_M5_TELEM_CHR_VAL);

/*
 * Company identifier 0xFFFF is reserved by the Bluetooth SIG for tests and
 * internal/experimental use; this is not a registered M5Stack/vendor ID.
 */
#define COMPANY_ID_TEST 0xFFFF

/* Packed, little-endian telemetry payload. See file header for field meaning. */
struct ble_telemetry {
	uint32_t uptime_s;
	int16_t bat_mv;
	int16_t accel_mg[3];
} __packed;

/* Latest telemetry, repacked by ble_update(). Read path copies from here. */
static struct ble_telemetry telem;

/* Manufacturer-data buffer = 2-byte company id (LE) + packed telemetry. */
static uint8_t mfg_data[2 + sizeof(struct ble_telemetry)];

static atomic_t conn_count = ATOMIC_INIT(0);
static atomic_t advertising = ATOMIC_INIT(0);

/* GATT read callback: hand back the current packed telemetry value. */
static ssize_t telem_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  void *buf, uint16_t len, uint16_t offset)
{
	ARG_UNUSED(attr);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &telem,
				 sizeof(telem));
}

/*
 * Custom primary service with one telemetry characteristic (READ + NOTIFY) and
 * its CCC. Attribute layout in telem_svc.attrs[]:
 *   [0] primary service declaration
 *   [1] characteristic declaration   <- notify start point (notifies [2])
 *   [2] characteristic value          (telem_read)
 *   [3] CCC descriptor
 */
BT_GATT_SERVICE_DEFINE(telem_svc,
	BT_GATT_PRIMARY_SERVICE(&telem_svc_uuid),
	BT_GATT_CHARACTERISTIC(&telem_chr_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ,
			       telem_read, NULL, NULL),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* Advertising data: flags + device name + manufacturer-specific telemetry. */
static struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg_data, sizeof(mfg_data)),
};

/*
 * Restart advertising from the system workqueue, NOT inline in disconnected():
 * calling bt_le_adv_start() directly in the disconnect callback can return
 * -EAGAIN because the just-closed connection context is not yet released.
 */
static void adv_work_handler(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		LOG_WRN("Advertising restart failed (%d)", err);
		atomic_set(&advertising, 0);
	} else {
		atomic_set(&advertising, 1);
		LOG_INF("BLE advertising restarted");
	}
}

static K_WORK_DEFINE(adv_work, adv_work_handler);

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_WRN("BLE connect failed (0x%02x)", err);
		return;
	}

	atomic_inc(&conn_count);
	/* The controller stops connectable advertising once a connection is up. */
	atomic_set(&advertising, 0);
	LOG_INF("BLE connected (conns=%ld)", (long)atomic_get(&conn_count));
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);

	if (atomic_get(&conn_count) > 0) {
		atomic_dec(&conn_count);
	}
	LOG_INF("BLE disconnected (reason 0x%02x, conns=%ld)", reason,
		(long)atomic_get(&conn_count));

	/*
	 * Connectable advertising is stopped by the controller while a
	 * connection is up. Restart it via the workqueue (inline bt_le_adv_start
	 * here can fail with -EAGAIN before the conn is fully freed); otherwise
	 * the device silently stops advertising after the first connect/disconnect.
	 */
	k_work_submit(&adv_work);
}

BT_CONN_CB_DEFINE(conn_cb) = {
	.connected = connected,
	.disconnected = disconnected,
};

/* Build the packed telemetry (and the mirrored manufacturer-data buffer). */
static void pack_telemetry(const struct app_status *s)
{
	int32_t bat = s->bat_mv;
	struct ble_telemetry t;

	t.uptime_s = sys_cpu_to_le32((uint32_t)(s->uptime_ms / 1000));

	if (bat > INT16_MAX) {
		bat = INT16_MAX;
	} else if (bat < INT16_MIN) {
		bat = INT16_MIN;
	}
	t.bat_mv = (int16_t)sys_cpu_to_le16((uint16_t)(int16_t)bat);

	for (int i = 0; i < 3; i++) {
		/* m/s^2 -> milli-g: mg = (val1 + val2/1e6) * 1000 / 9.80665. */
		int64_t micro_mss = (int64_t)s->accel[i].val1 * 1000000 +
				    s->accel[i].val2;
		int64_t mg = (micro_mss * 1000) / 9806650;

		if (mg > INT16_MAX) {
			mg = INT16_MAX;
		} else if (mg < INT16_MIN) {
			mg = INT16_MIN;
		}
		t.accel_mg[i] = (int16_t)sys_cpu_to_le16((uint16_t)(int16_t)mg);
	}

	telem = t;

	/* Manufacturer data = company id (LE) followed by the same payload. */
	sys_put_le16(COMPANY_ID_TEST, &mfg_data[0]);
	memcpy(&mfg_data[2], &t, sizeof(t));
}

int ble_init(void)
{
	int err;

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
		return err;
	}

	/* Seed the payload so the first advert/read is well-formed. */
	struct app_status zero = {0};

	pack_telemetry(&zero);

	/*
	 * Connectable + scannable legacy advertising. In Zephyr 4.4 the legacy
	 * BT_LE_ADV_CONN / BT_LE_ADV_CONN_NAME macros were removed; the GAP-
	 * recommended connectable preset is BT_LE_ADV_CONN_FAST_1 (options =
	 * BT_LE_ADV_OPT_CONN, undirected). It is connectable and scannable, and
	 * does NOT auto-insert the name, so we provide the name in the AD above.
	 */
	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad),
			      NULL, 0);
	if (err) {
		LOG_ERR("Advertising start failed (%d)", err);
		return err;
	}

	atomic_set(&advertising, 1);
	LOG_INF("BLE advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);
	return 0;
}

void ble_update(const struct app_status *s)
{
	static int64_t last_ms;
	int64_t now = k_uptime_get();

	/* Throttle to ~1 Hz even though the main loop calls us every tick. */
	if (last_ms != 0 && (now - last_ms) < 1000) {
		return;
	}
	last_ms = now;

	pack_telemetry(s);

	/*
	 * Refresh the manufacturer-data payload only while advertising; the
	 * controller stops connectable advertising while a connection is up
	 * (notifications below still reach the connected client).
	 */
	if (atomic_get(&advertising)) {
		(void)bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);
	}

	/*
	 * Notify subscribed clients. Passing the characteristic declaration
	 * (attrs[1]) lets the stack notify the value attribute; conn == NULL
	 * notifies every subscriber and is a no-op when no CCC is enabled.
	 */
	(void)bt_gatt_notify(NULL, &telem_svc.attrs[1], &telem, sizeof(telem));
}

bool ble_is_advertising(void)
{
	return atomic_get(&advertising) != 0;
}

uint32_t ble_conn_count(void)
{
	return (uint32_t)atomic_get(&conn_count);
}

#endif /* CONFIG_APP_BLE */
