# Sources and Source-derived Claims

Research snapshot date: 2026-05-30.

## M5Stack StickS3 hardware facts

- Official docs: https://docs.m5stack.com/en/core/StickS3
  - ESP32-S3-PICO-1-N8R8, 8MB Flash, 8MB PSRAM.
  - LCD ST7789P3, 135x240.
  - BMI270 IMU.
  - ES8311 audio codec, MEMS microphone, speaker amplifier.
  - IR TX/RX, battery, expansion interfaces.
  - PinMap used in this repo.
- Product page: https://shop.m5stack.com/products/m5sticks3-esp32s3-mini-iot-dev-kit
  - Product specs and usage warnings.
- Launch post: https://shop.m5stack.com/blogs/news/m5stack-launches-sticks3-an-enhanced-compact-controller-for-iot-projects
  - Launch date and product positioning.

## Zephyr facts

- Supported boards list: https://docs.zephyrproject.org/latest/boards/index.html
- Board porting guide: https://docs.zephyrproject.org/latest/hardware/porting/board_porting.html
- Application development / example-application: https://docs.zephyrproject.org/latest/develop/application/index.html
- Releases: https://docs.zephyrproject.org/latest/releases/index.html
- Zephyr 4.4.0 release notes: https://docs.zephyrproject.org/latest/releases/release-notes-4.4.html
- Twister: https://docs.zephyrproject.org/latest/develop/test/twister.html
- Devicetree style: https://docs.zephyrproject.org/latest/contribute/style/devicetree.html
- ST7789V binding: https://docs.zephyrproject.org/latest/build/dts/api/bindings/display/sitronix%2Cst7789v.html
- BMI270 binding: https://docs.zephyrproject.org/latest/build/dts/api/bindings/sensor/bosch%2Cbmi270.html

## Espressif / ESP32 Zephyr facts

- Espressif Zephyr support status: https://developer.espressif.com/software/zephyr-support-status/
- ESP-Zephyr: https://www.espressif.com/en/sdks/esp-zephyr

## M5PM1 PMIC / LCD power + ESP32-S3 flashing (added 2026-05-30)

- M5PM1 PMIC library + datasheet (register map, I2C 0x6e, DEVICE_ID 0x50):
  https://github.com/m5stack/M5PM1 (src/M5PM1.h, docs/M5PM1_Datasheet_EN.pdf)
- M5GFX board init (StickS3 LCD power-up: PYG2/L3B sequence, ST7789P3 offsets
  x=52/y=40, inversion, backlight G38 PWM): https://github.com/m5stack/M5GFX (src/M5GFX.cpp)
- M5Unified (PYG0-PYG4 roles): https://github.com/m5stack/M5Unified
- StickS3 low-power / M5PM1 doc (L0-L3B power levels): https://docs.m5stack.com/en/arduino/m5sticks3/m5pm1
- esptool troubleshooting + advanced options (USB-Serial/JTAG `--after watchdog-reset`):
  https://docs.espressif.com/projects/esptool/en/latest/esp32s3/troubleshooting.html
  https://docs.espressif.com/projects/esptool/en/latest/esp32s3/esptool/advanced-options.html
- Zephyr regulator subsystem + reference drivers (axp192, tps55287, fixed):
  https://docs.zephyrproject.org/latest/hardware/peripherals/regulators.html
- Zephyr out-of-tree modules: https://docs.zephyrproject.org/latest/develop/modules.html
