.. zephyr:board:: m5stack_sticks3

Overview
********

M5Stack StickS3 is a compact ESP32-S3 based IoT controller with LCD, IMU,
audio, IR, buttons, battery, HY2.0-4P and Hat2-Bus expansion.

This documentation is an initial out-of-tree board-port draft. Do not treat it
as upstream support until the build and hardware validation matrix is complete.

Hardware
********

* SoC: ESP32-S3-PICO-1-N8R8
* Flash: 8MB
* PSRAM: 8MB
* Display: ST7789P3, 135x240
* IMU: BMI270
* Audio codec: ES8311
* Buttons: KEY1, KEY2
* IR: TX/RX
* Battery: 250mAh

Supported Features
******************

+-------------+----------+-------------------------------+
| Feature     | Status   | Notes                         |
+=============+==========+===============================+
| Boot        | pending  | Requires physical validation  |
+-------------+----------+-------------------------------+
| Console     | pending  | Requires physical validation  |
+-------------+----------+-------------------------------+
| Buttons     | pending  | G11/G12 from vendor PinMap    |
+-------------+----------+-------------------------------+
| LCD         | pending  | ST7789P3 via ST7789V path     |
+-------------+----------+-------------------------------+
| BMI270      | pending  | I2C address 0x68              |
+-------------+----------+-------------------------------+
| M5PM1       | pending  | I2C address 0x6e              |
+-------------+----------+-------------------------------+
| ES8311      | pending  | I2C address 0x18 + I2S        |
+-------------+----------+-------------------------------+
| IR          | pending  | ESP32 RMT strategy to verify  |
+-------------+----------+-------------------------------+

Programming and Debugging
*************************

The M5Stack documentation says download mode is entered by connecting USB and
pressing/holding reset until the internal green LED flashes.

Build example:

.. code-block:: console

   west build -p always -b m5stack_sticks3 app
   west flash
