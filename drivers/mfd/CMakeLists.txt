# SPDX-License-Identifier: Apache-2.0
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_EMUL_M5PM1_MFD emul_m5pm1.c)
