# SPDX-License-Identifier: Apache-2.0
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_MFD_M5PM1 mfd_m5pm1.c)
zephyr_library_sources_ifdef(CONFIG_EMUL_M5PM1_MFD emul_m5pm1.c)
