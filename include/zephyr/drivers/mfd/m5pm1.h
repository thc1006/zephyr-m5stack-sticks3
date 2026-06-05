/*
 * Copyright (c) 2026 The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Vendored from Zephyr PR #109961 (commit b9dde93c9a8173a884a3aabd7f08bc23260ae9e7),
 * Apache-2.0. Interim copy; #109961 MERGED upstream 2026-06-03 (drop when Zephyr is bumped past 4.4.0).
 * See docs/07_UPSTREAM_PLAN.md.
 */

/**
 * @file
 * @ingroup mfd_interface_m5pm1
 * @brief Header file for the M5Stack M5PM1 MFD driver.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MFD_M5PM1_H_
#define ZEPHYR_INCLUDE_DRIVERS_MFD_M5PM1_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup mfd_interface_m5pm1 MFD M5PM1 Interface
 * @ingroup mfd_interfaces
 * @brief M5Stack M5PM1 power management companion MCU interface.
 *
 * The M5PM1 is a small companion microcontroller used on M5Stack boards to provide power management
 * (battery charging, switchable rails), a small bank of GPIOs and an on-chip ADC. This MFD driver
 * owns the I2C transport and serializes register access; sibling GPIO, ADC and regulator drivers
 * implement their domain-specific functionality on top of these primitives.
 * @{
 */

/**
 * @brief Read a single 8-bit M5PM1 register.
 *
 * @param dev M5PM1 MFD device.
 * @param reg Register address.
 * @param[out] val Pointer that receives the register value.
 *
 * @retval 0 On success.
 * @retval -errno On I2C transfer error (see i2c_reg_read_byte_dt()).
 */
int mfd_m5pm1_read_reg(const struct device *dev, uint8_t reg, uint8_t *val);

/**
 * @brief Write a single 8-bit M5PM1 register.
 *
 * @param dev M5PM1 MFD device.
 * @param reg Register address.
 * @param val Value to write.
 *
 * @retval 0 On success.
 * @retval -errno On I2C transfer error (see i2c_reg_write_byte_dt()).
 */
int mfd_m5pm1_write_reg(const struct device *dev, uint8_t reg, uint8_t val);

/**
 * @brief Read-modify-write selected bits of an M5PM1 register.
 *
 * Performs an atomic (mutex-protected) read-modify-write of @p reg, replacing the bits selected by
 * @p mask with the corresponding bits from @p val.
 *
 * @param dev M5PM1 MFD device.
 * @param reg Register address.
 * @param mask Bitmask of bits to update.
 * @param val New value for the bits selected by @p mask (other bits ignored).
 *
 * @retval 0 On success.
 * @retval -errno On I2C transfer error (see i2c_reg_update_byte_dt()).
 */
int mfd_m5pm1_update_reg(const struct device *dev, uint8_t reg, uint8_t mask, uint8_t val);

/**
 * @brief Read multiple consecutive M5PM1 registers in a single I2C transaction.
 *
 * Uses a single repeated-start I2C transfer so the device sees the read as atomic, which matters
 * for register pairs latched on the low-byte read (e.g. ADC sample registers).
 *
 * @param dev M5PM1 MFD device.
 * @param reg Address of the first register to read.
 * @param[out] buf Buffer that receives @p len consecutive register values.
 * @param len Number of bytes to read.
 *
 * @retval 0 On success.
 * @retval -errno On I2C transfer error (see i2c_burst_read_dt()).
 */
int mfd_m5pm1_burst_read(const struct device *dev, uint8_t reg, uint8_t *buf, size_t len);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MFD_M5PM1_H_ */
