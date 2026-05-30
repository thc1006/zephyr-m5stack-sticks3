/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/printk.h>

#define SLEEP_TIME_MS 1000

/* KEY1 = G11, KEY2 = G12 (hardware-confirmed; active-low, pull-up). */
#define KEY1_PIN 11
#define KEY2_PIN 12

static const char *imu_state = "absent";
static const char *disp_state = "absent";

/* gpio-keys also reports KEY1/KEY2 via the input subsystem (zephyr,code). */
static void input_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->type == INPUT_EV_KEY) {
		printk("BUTTON code=%u %s\n", evt->code,
		       evt->value ? "PRESSED" : "released");
	}
}
INPUT_CALLBACK_DEFINE(NULL, input_cb, NULL);

#if DT_NODE_HAS_STATUS(DT_CHOSEN(zephyr_display), okay)
#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
#define LCD_W DT_PROP(DISPLAY_NODE, width)
#define LCD_H DT_PROP(DISPLAY_NODE, height)

static uint8_t line_buf[LCD_W * 2];

static void fill_screen(const struct device *disp, uint16_t color)
{
	struct display_buffer_descriptor desc = {
		.buf_size = sizeof(line_buf),
		.width = LCD_W,
		.height = 1,
		.pitch = LCD_W,
	};

	for (int i = 0; i < LCD_W; i++) {
		line_buf[2 * i] = color >> 8;
		line_buf[2 * i + 1] = color & 0xff;
	}
	for (int y = 0; y < LCD_H; y++) {
		if (display_write(disp, 0, y, &desc, line_buf) != 0) {
			break;
		}
	}
}
#endif

int main(void)
{
	printk("M5StickS3 Zephyr validation app\n");
	printk("Board: %s\n", CONFIG_BOARD);

	const struct device *g0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	const struct device *bl = DEVICE_DT_GET(DT_NODELABEL(lcd_backlight));
	uint32_t tick = 0;

#if DT_NODE_HAS_STATUS(DT_ALIAS(imu0), okay)
	const struct device *imu = DEVICE_DT_GET(DT_ALIAS(imu0));

	if (device_is_ready(imu)) {
		struct sensor_value v;

		imu_state = "ready";
		v.val1 = 2;
		v.val2 = 0;
		sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_FULL_SCALE, &v);
		v.val1 = 100;
		v.val2 = 0;
		sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ,
				SENSOR_ATTR_SAMPLING_FREQUENCY, &v);
	} else {
		imu_state = "not-ready";
	}
#endif

#if DT_NODE_HAS_STATUS(DT_CHOSEN(zephyr_display), okay)
	const struct device *disp = DEVICE_DT_GET(DISPLAY_NODE);
	bool disp_ok = device_is_ready(disp);

	if (disp_ok) {
		display_set_pixel_format(disp, PIXEL_FORMAT_RGB_565);
		display_blanking_off(disp);
		disp_state = "ready";
		printk("Display: ready (%dx%d)\n", LCD_W, LCD_H);
	} else {
		disp_state = "not-ready";
		printk("Display: not ready\n");
	}
	static const uint16_t colors[] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
	int ci = 0;
#endif

	while (1) {
		int k1 = gpio_pin_get_raw(g0, KEY1_PIN);
		int k2 = gpio_pin_get_raw(g0, KEY2_PIN);

#if DT_NODE_HAS_STATUS(DT_CHOSEN(zephyr_display), okay)
		if (disp_ok) {
			fill_screen(disp, colors[ci]);
			ci = (ci + 1) % ARRAY_SIZE(colors);
		}
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(imu0), okay)
		struct sensor_value acc[3] = {0};

		if (device_is_ready(imu) && sensor_sample_fetch(imu) == 0) {
			sensor_channel_get(imu, SENSOR_CHAN_ACCEL_XYZ, acc);
		}
		printk("M5StickS3 alive uptime_ms=%lld disp=%s KEY1=%d KEY2=%d "
		       "accel=[%d.%06d %d.%06d %d.%06d]\n",
		       k_uptime_get(), disp_state, k1, k2,
		       acc[0].val1, abs(acc[0].val2), acc[1].val1, abs(acc[1].val2),
		       acc[2].val1, abs(acc[2].val2));
#else
		printk("M5StickS3 alive uptime_ms=%lld disp=%s KEY1=%d KEY2=%d imu=%s\n",
		       k_uptime_get(), disp_state, k1, k2, imu_state);
#endif
		k_msleep(SLEEP_TIME_MS);

		/*
		 * B-3: every ~5 s blink the backlight via regulator_disable/enable
		 * to prove regulator power control works on real hardware (screen
		 * goes fully dark, then the image returns intact).
		 */
		if ((++tick % 5U) == 0U && device_is_ready(bl)) {
			printk(">>> Backlight OFF (regulator_disable) - screen should go DARK\n");
			(void)regulator_disable(bl);
			k_msleep(1500);
			(void)regulator_enable(bl);
			printk(">>> Backlight ON (regulator_enable) - screen should be VISIBLE\n");
		}
	}

	return 0;
}
