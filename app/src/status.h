/* SPDX-License-Identifier: Apache-2.0 */
#ifndef M5STICKS3_STATUS_H
#define M5STICKS3_STATUS_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/sensor.h>

/* Telemetry gathered once per main-loop iteration. */
struct app_status {
	int64_t uptime_ms;
	int bat_mv;                  /* -1 = not available (until P3) */
	bool imu_ok;
	struct sensor_value accel[3]; /* x/y/z, m/s^2 */
};

void status_init(void);
void status_sample(struct app_status *s);

#endif /* M5STICKS3_STATUS_H */
