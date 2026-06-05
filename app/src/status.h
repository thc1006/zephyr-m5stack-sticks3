/* SPDX-License-Identifier: Apache-2.0 */
#ifndef M5STICKS3_STATUS_H
#define M5STICKS3_STATUS_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/sensor.h>

/* Telemetry gathered once per main-loop iteration. */
struct app_status {
	int64_t uptime_ms;
	int bat_mv;                  /* VBAT in mV; -1 = not available */
	int soc_pct;                 /* battery state-of-charge %; -1 = n/a (issue #8) */
	int vin_mv;                  /* VIN (5V/USB input) in mV; -1 = n/a */
	bool imu_ok;
	struct sensor_value accel[3]; /* x/y/z, m/s^2 */
};

void status_init(void);
void status_sample(struct app_status *s);

#endif /* M5STICKS3_STATUS_H */
