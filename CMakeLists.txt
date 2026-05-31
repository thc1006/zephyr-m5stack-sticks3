# SPDX-License-Identifier: Apache-2.0
# Out-of-tree Zephyr module: M5StickS3 custom drivers (M5PM1 PMIC).
# Expose the vendored MFD interim header so <zephyr/drivers/mfd/m5pm1.h> resolves.
zephyr_include_directories(include)
add_subdirectory(drivers)
