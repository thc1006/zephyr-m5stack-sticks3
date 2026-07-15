/*
 * Copyright (c) 2026 Hsiu-Chi Tsai
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Test backend for the ES8311 I2C emulator.
 *
 * These hooks let a ztest drive the emulated codec into states and failure modes the driver
 * has to survive but that an ordinary register store cannot express: a part holding its
 * register file down, a bus whose reads fail while its writes land, a write to one specific
 * register that fails, a caller parked mid-transfer so a second thread can race it. They are
 * declared here, in one place, so that the emulator's definitions and the tests' uses are
 * checked against a single source rather than a hand-copied set of extern prototypes that can
 * drift. Both drivers/audio/emul_es8311.c and the test include this header.
 *
 * The header is private to the ES8311 test: the emulator is only ever compiled by it.
 */

#ifndef ZEPHYR_TESTS_DRIVERS_AUDIO_ES8311_EMUL_ES8311_H_
#define ZEPHYR_TESTS_DRIVERS_AUDIO_ES8311_EMUL_ES8311_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/emul.h>
#include <zephyr/kernel.h>

/* Fail the next @p n transfers with -EIO, whatever they are. */
void emul_es8311_set_fail(const struct emul *target, int n);

/*
 * Fail transfer number @p idx (0-based) and let the ones before it through; pass a negative
 * value to disarm. Fires once. Reaches a failure in the MIDDLE of a sequence, which
 * emul_es8311_set_fail() cannot.
 */
void emul_es8311_fail_at(const struct emul *target, int idx);

/*
 * Fail every write to register @p reg, and only to @p reg; pass a negative value to disarm.
 * Says what it means -- "break the write to THIS register" -- and keeps saying it when a read
 * added upstream would have re-aimed a by-index fault at a different write.
 */
void emul_es8311_fail_write_to(const struct emul *target, int reg);

/*
 * Break every READ and let every WRITE through, until disarmed. A read on this bus is a
 * write-then-read with a repeated start, which a controller can fail while a plain register
 * write still works -- exactly the failure that turns a read-modify-write mute into no mute.
 */
void emul_es8311_fail_reads(const struct emul *target, bool fail);

/*
 * Hold, or release, INI_REG (0xFA bit 0) as a LEVEL: while held, every write to any other
 * register is silently discarded and every read returns 0x00, the way the vendor Linux driver
 * leaves the part at shutdown.
 */
void emul_es8311_set_ini_hold(const struct emul *target, bool hold);

/* Override the chip-id registers (0xFD/0xFE), to exercise the driver's identity rejection. */
void emul_es8311_set_chip_id(const struct emul *target, uint8_t id1, uint8_t id2);

/* The ordered write log: addresses, values, and its length. reset_log() clears it. */
void emul_es8311_reset_log(const struct emul *target);
int emul_es8311_write_count(const struct emul *target);
int emul_es8311_write_at(const struct emul *target, int idx);
int emul_es8311_wval_at(const struct emul *target, int idx);

/*
 * Concurrency hook: the next write to @p reg blocks inside the transfer, in the caller's own
 * thread and holding whatever locks it holds, until release() -- so a second thread can be
 * driven into the middle of a register sequence. wait_paused() blocks until a caller parks.
 */
void emul_es8311_pause_before(const struct emul *target, uint8_t reg);
int emul_es8311_wait_paused(const struct emul *target, k_timeout_t timeout);
void emul_es8311_release(const struct emul *target);

/* Put the emulated part back exactly as it comes up: clean register file, every hook disarmed. */
void emul_es8311_reset(const struct emul *target);

#endif /* ZEPHYR_TESTS_DRIVERS_AUDIO_ES8311_EMUL_ES8311_H_ */
