/* SPDX-License-Identifier: Apache-2.0 */
#ifndef M5STICKS3_IR_H
#define M5STICKS3_IR_H

#include <stdbool.h>
#include <stdint.h>

/*
 * IR (NEC) transmit + receive, gated behind CONFIG_APP_IR. When the option is
 * off these functions are not declared and the source is not compiled, so the
 * default build is unchanged. Zephyr 4.4 has no ESP32 RMT driver, so TX uses the
 * LEDC carrier (G46) and RX uses MCPWM input capture (G42).
 */
#ifdef CONFIG_APP_IR

/* Bring up the IR TX carrier (LEDC) and RX capture (MCPWM). 0 on success. */
int ir_init(void);

/* True once ir_init() has succeeded and IR is usable. */
bool ir_ready(void);

#endif /* CONFIG_APP_IR */

#endif /* M5STICKS3_IR_H */
