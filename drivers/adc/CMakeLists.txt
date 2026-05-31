# SPDX-License-Identifier: Apache-2.0
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_ADC_M5PM1 adc_m5pm1.c)
