/* SPDX-License-Identifier: Apache-2.0 */
#include "ir.h"

#ifdef CONFIG_APP_IR

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ir, LOG_LEVEL_INF);

static bool ready;

int ir_init(void)
{
	/*
	 * IR-P1 scaffold only. The LEDC TX carrier + NEC envelope (IR-P3) and the
	 * MCPWM RX capture + decode (IR-P4) are added in later phases; this keeps
	 * the gated build wiring (Kconfig / overlay / page) verifiable on its own.
	 */
	ready = false;
	LOG_INF("ir_init scaffold (TX/RX bring-up pending)");
	return 0;
}

bool ir_ready(void)
{
	return ready;
}

#endif /* CONFIG_APP_IR */
