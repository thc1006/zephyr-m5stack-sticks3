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

/* Transmit one NEC frame (blocking, ~tens of ms; holds off preemption). */
void ir_tx_nec(uint8_t addr, uint8_t cmd);

/* Transmit the fixed demo NEC frame used by the IR page. */
void ir_tx_test(void);

/* Number of NEC frames transmitted so far (for the UI). */
uint32_t ir_tx_count(void);

/* Last NEC frame decoded by RX; returns false if nothing has been decoded. */
bool ir_rx_last(uint8_t *addr, uint8_t *cmd);

/* Number of NEC frames decoded by RX so far (for the UI). */
uint32_t ir_rx_count(void);

/*
 * Total IR edges seen on the RX line; nonzero for any remote of any protocol,
 * even one the NEC decoder does not recognise (a generic "IR received" signal).
 */
uint32_t ir_rx_edges(void);

#endif /* CONFIG_APP_IR */

#endif /* M5STICKS3_IR_H */
