# SPDX-License-Identifier: Apache-2.0
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_REGULATOR_M5PM1 regulator_m5pm1.c)
zephyr_library_sources_ifdef(CONFIG_EMUL_M5PM1 emul_m5pm1.c)
