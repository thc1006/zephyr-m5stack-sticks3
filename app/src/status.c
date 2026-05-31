/* SPDX-License-Identifier: Apache-2.0 */
#include "status.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>

#if DT_NODE_HAS_STATUS(DT_ALIAS(imu0), okay)
static const struct device *const imu = DEVICE_DT_GET(DT_ALIAS(imu0));
#else
static const struct device *const imu;
#endif

/*
 * Battery voltage via the M5PM1 ADC (logical channel 1 = VBAT). The PMIC ADC
 * registers report millivolts directly (12-bit), so the value placed in the
 * sample buffer by adc_read() is already in mV -- no adc_raw_to_millivolts()
 * conversion is needed for VBAT.
 */
#define VBAT_CHANNEL 1U

#if DT_NODE_HAS_STATUS(DT_NODELABEL(m5pm1_adc), okay)
#define HAVE_VBAT 1
static const struct device *const adc = DEVICE_DT_GET(DT_NODELABEL(m5pm1_adc));

static const struct adc_channel_cfg vbat_ch_cfg = {
	.gain = ADC_GAIN_1,
	.reference = ADC_REF_INTERNAL,
	.acquisition_time = ADC_ACQ_TIME_DEFAULT,
	.channel_id = VBAT_CHANNEL,
};
#else
#define HAVE_VBAT 0
#endif

void status_init(void)
{
	if (imu != NULL && device_is_ready(imu)) {
		struct sensor_value v;

		v.val1 = 2;
		v.val2 = 0;
		sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ,
				SENSOR_ATTR_FULL_SCALE, &v);
		v.val1 = 100;
		v.val2 = 0;
		sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ,
				SENSOR_ATTR_SAMPLING_FREQUENCY, &v);
	}

#if HAVE_VBAT
	if (device_is_ready(adc)) {
		(void)adc_channel_setup(adc, &vbat_ch_cfg);
	}
#endif
}

static int read_vbat_mv(void)
{
#if HAVE_VBAT
	uint16_t sample = 0;
	struct adc_sequence seq = {
		.channels = BIT(VBAT_CHANNEL),
		.buffer = &sample,
		.buffer_size = sizeof(sample),
		.resolution = 12,
	};

	if (!device_is_ready(adc)) {
		return -1;
	}

	if (adc_read(adc, &seq) != 0) {
		return -1;
	}

	/* M5PM1 VBAT register is already in millivolts. */
	return (int)sample;
#else
	return -1;
#endif
}

void status_sample(struct app_status *s)
{
	s->uptime_ms = k_uptime_get();
	s->bat_mv = read_vbat_mv();
	s->imu_ok = false;
	for (int i = 0; i < 3; i++) {
		s->accel[i].val1 = 0;
		s->accel[i].val2 = 0;
	}

	if (imu != NULL && device_is_ready(imu) && sensor_sample_fetch(imu) == 0) {
		if (sensor_channel_get(imu, SENSOR_CHAN_ACCEL_XYZ, s->accel) == 0) {
			s->imu_ok = true;
		}
	}
}
