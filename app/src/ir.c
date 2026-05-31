/* SPDX-License-Identifier: Apache-2.0 */
#include "ir.h"

#ifdef CONFIG_APP_IR

#include "nec.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ir, LOG_LEVEL_INF);

/* RX is suspended while transmitting so the TX burst is neither self-received
 * nor jittered by the RX edge ISR (defined in the RX section below). */
static void ir_rx_suspend(void);
static void ir_rx_resume(void);

/*
 * IR on stock peripherals (Zephyr 4.4 has no ESP32 RMT driver).
 *
 * TX (G46): the LEDC PWM generates a ~38 kHz carrier; the NEC envelope is made
 * by gating the carrier duty between ~1/3 (mark) and 0 (space) with k_busy_wait
 * between transitions, under k_sched_lock so a context switch cannot stretch a
 * mark/space.
 *
 * RX (G42): a plain GPIO edge interrupt. The Zephyr ESP32 MCPWM capture path
 * drops edges during the fast 67 ms NEC burst (confirmed on hardware), so each
 * edge is timestamped with the cycle counter instead; the level that just ended
 * becomes a mark (low) or space (high), and (mark, space) pairs feed
 * nec_decode(). A TX->RX loopback on the same device decodes correctly.
 */

/* ------------------------------- TX (LEDC) ------------------------------- */

static const struct device *const ledc = DEVICE_DT_GET(DT_NODELABEL(ledc0));

#define IR_TX_CHANNEL        0U
#define IR_CARRIER_PERIOD_NS 26316U /* ~38.0 kHz */
#define IR_CARRIER_PULSE_NS  8772U  /* ~1/3 duty */

/* The demo IR page transmits this fixed NEC code. */
#define IR_TEST_ADDR 0x04U
#define IR_TEST_CMD  0x1BU

static bool ready;
static uint32_t tx_count;

static inline void carrier_on(void)
{
	(void)pwm_set(ledc, IR_TX_CHANNEL, IR_CARRIER_PERIOD_NS,
		      IR_CARRIER_PULSE_NS, 0);
}

static inline void carrier_off(void)
{
	(void)pwm_set(ledc, IR_TX_CHANNEL, IR_CARRIER_PERIOD_NS, 0U, 0);
}

static inline void mark(uint32_t us)
{
	carrier_on();
	k_busy_wait(us);
}

static inline void space(uint32_t us)
{
	carrier_off();
	k_busy_wait(us);
}

void ir_tx_nec(uint8_t addr, uint8_t cmd)
{
	uint32_t frame;

	if (!ready) {
		return;
	}

	frame = nec_encode(addr, cmd);

	/*
	 * Suspend RX for the burst (we must not receive our own carrier, and the
	 * RX edge ISR would otherwise jitter the busy-wait timing), and hold off
	 * thread preemption (~tens of ms, like the audio beep) so a context switch
	 * cannot stretch a mark/space. Interrupts still run; NEC's ~+/-20%
	 * tolerance on the 560 us base unit absorbs that jitter.
	 */
	ir_rx_suspend();
	k_sched_lock();
	mark(NEC_LEADER_MARK_US);
	space(NEC_LEADER_SPACE_US);
	for (uint32_t i = 0; i < 32U; i++) {
		mark(NEC_BIT_MARK_US);
		space(((frame >> i) & 1U) ? NEC_ONE_SPACE_US : NEC_ZERO_SPACE_US);
	}
	mark(NEC_STOP_MARK_US);
	carrier_off();
	k_sched_unlock();
	ir_rx_resume();

	tx_count++;
	LOG_INF("ir_tx addr=0x%02x cmd=0x%02x (#%u)", addr, cmd,
		(unsigned int)tx_count);
}

void ir_tx_test(void)
{
	ir_tx_nec(IR_TEST_ADDR, IR_TEST_CMD);
}

uint32_t ir_tx_count(void)
{
	return tx_count;
}

/* ------------------------------- RX (GPIO) ------------------------------- */

#define IR_RX_NODE DT_PATH(zephyr_user)

#if DT_NODE_HAS_PROP(IR_RX_NODE, ir_rx_gpios)

static const struct gpio_dt_spec ir_rx = GPIO_DT_SPEC_GET(IR_RX_NODE, ir_rx_gpios);
static struct gpio_callback ir_rx_cb;

static struct nec_event rx_buf[NEC_FRAME_EVENTS];
static uint32_t rx_idx;
static uint32_t last_edge_cyc;
static uint16_t pending_mark_us;
static bool have_pending_mark;
static volatile uint32_t rx_count;
static volatile uint8_t rx_addr;
static volatile uint8_t rx_cmd;
static volatile uint32_t rx_edges; /* IR activity: total RX-line edges (any protocol) */

static inline uint16_t dcyc_to_us(uint32_t dcyc)
{
	uint32_t us = k_cyc_to_us_floor32(dcyc);

	return (us > 0xFFFFU) ? 0xFFFFU : (uint16_t)us;
}

static void process_pair(uint16_t mark_us, uint16_t space_us)
{
	struct nec_frame f;

	/*
	 * Only a real NEC leader (9 ms mark + 4.5 ms space, or 2.25 ms repeat)
	 * starts a frame; ambient flicker (~9 ms mark, tiny space) is rejected.
	 */
	if (mark_us >= 7000U && mark_us <= 11000U) {
		if (space_us >= 1800U && space_us <= 2700U) {
			rx_count++; /* repeat frame: keep the last addr/cmd */
			rx_idx = 0U;
			return;
		}
		if (space_us >= 3500U && space_us <= 5500U) {
			rx_buf[0].mark_us = mark_us;
			rx_buf[0].space_us = space_us;
			rx_idx = 1U;
			return;
		}
		rx_idx = 0U;
		return;
	}

	if (rx_idx == 0U) {
		return; /* no leader yet: ignore stray/ambient edges */
	}

	if (rx_idx < NEC_FRAME_EVENTS) {
		rx_buf[rx_idx].mark_us = mark_us;
		rx_buf[rx_idx].space_us = space_us;
		rx_idx++;
	}

	if (rx_idx >= NEC_FRAME_EVENTS) {
		if (nec_decode(rx_buf, NEC_FRAME_EVENTS, &f) == NEC_DECODE_OK) {
			rx_addr = f.addr;
			rx_cmd = f.cmd;
			rx_count++;
		}
		rx_idx = 0U;
	}
}

static void ir_rx_edge(const struct device *port, struct gpio_callback *cb,
		       uint32_t pins)
{
	uint32_t now = k_cycle_get_32();
	uint16_t dt_us = dcyc_to_us(now - last_edge_cyc);
	int level;

	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	rx_edges++;
	last_edge_cyc = now;
	level = gpio_pin_get_dt(&ir_rx); /* physical level (active-high spec) */

	if (level == 1) {
		/* rising edge: a LOW (mark) just ended */
		pending_mark_us = dt_us;
		have_pending_mark = true;
	} else if (have_pending_mark) {
		/* falling edge: a HIGH (space) just ended; complete the pair */
		process_pair(pending_mark_us, dt_us);
		have_pending_mark = false;
	}
}

static void ir_rx_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&ir_rx)) {
		LOG_ERR("IR RX gpio not ready; TX still available");
		return;
	}

	ret = gpio_pin_configure_dt(&ir_rx, GPIO_INPUT);
	if (ret == 0) {
		gpio_init_callback(&ir_rx_cb, ir_rx_edge, BIT(ir_rx.pin));
		ret = gpio_add_callback(ir_rx.port, &ir_rx_cb);
	}
	if (ret == 0) {
		ret = gpio_pin_interrupt_configure_dt(&ir_rx, GPIO_INT_EDGE_BOTH);
	}
	last_edge_cyc = k_cycle_get_32();

	if (ret != 0) {
		LOG_ERR("IR RX gpio interrupt setup failed: %d", ret);
	} else {
		LOG_INF("IR RX on GPIO edge interrupt (G42)");
	}
}

static void ir_rx_suspend(void)
{
	(void)gpio_pin_interrupt_configure_dt(&ir_rx, GPIO_INT_DISABLE);
}

static void ir_rx_resume(void)
{
	rx_idx = 0U;
	have_pending_mark = false;
	last_edge_cyc = k_cycle_get_32();
	(void)gpio_pin_interrupt_configure_dt(&ir_rx, GPIO_INT_EDGE_BOTH);
}

uint32_t ir_rx_count(void)
{
	return rx_count;
}

bool ir_rx_last(uint8_t *addr, uint8_t *cmd)
{
	if (rx_count == 0U) {
		return false;
	}
	if (addr != NULL) {
		*addr = rx_addr;
	}
	if (cmd != NULL) {
		*cmd = rx_cmd;
	}
	return true;
}

uint32_t ir_rx_edges(void)
{
	return rx_edges;
}

#else /* no ir-rx-gpios */

static void ir_rx_init(void)
{
}

static void ir_rx_suspend(void)
{
}

static void ir_rx_resume(void)
{
}

uint32_t ir_rx_count(void)
{
	return 0U;
}

bool ir_rx_last(uint8_t *addr, uint8_t *cmd)
{
	ARG_UNUSED(addr);
	ARG_UNUSED(cmd);
	return false;
}

uint32_t ir_rx_edges(void)
{
	return 0U;
}

#endif /* ir-rx-gpios */

/* --------------------------------- init ---------------------------------- */

int ir_init(void)
{
	if (!device_is_ready(ledc)) {
		LOG_ERR("LEDC (IR TX) not ready");
		return -ENODEV;
	}

	carrier_off(); /* idle low (LED off) */
	ready = true;
	LOG_INF("ir_init OK (TX on LEDC ch%u, ~38 kHz carrier)", IR_TX_CHANNEL);

	ir_rx_init();

	return 0;
}

bool ir_ready(void)
{
	return ready;
}

#endif /* CONFIG_APP_IR */
