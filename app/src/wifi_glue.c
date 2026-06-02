/* SPDX-License-Identifier: Apache-2.0 */

#include "wifi_glue.h"
#include "wifi.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(wifi_glue, LOG_LEVEL_INF);

#define WIFI_GLUE_MAX_APS 24U
#define WIFI_EVENTS                                                            \
	(NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE |                \
	 NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT)

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;
static struct net_if *wifi_iface;

static K_MUTEX_DEFINE(lock);
static struct wifi_ap aps[WIFI_GLUE_MAX_APS];
static struct wifi_ap snap[WIFI_GLUE_MAX_APS]; /* printk copy; net_mgmt thread only */
static size_t ap_count;
static enum wifi_glue_state state = WIFI_GLUE_IDLE;

/*
 * Connection state. conn_fsm mirrors the link for the UI only: the driver's
 * bundled supplicant owns (re)connect and CONFIG_ESP32_WIFI_STA_AUTO_DHCPV4 owns
 * DHCP, so the FSM never drives retries (wifi_fsm_should_retry is unused here).
 */
static struct wifi_fsm conn_fsm;
static char ip_str[NET_IPV4_ADDR_LEN];
static bool have_ip;

/*
 * The glue passes enum wifi_sec straight to Zephyr as enum wifi_security_type
 * (see wifi.h); assert the values still line up so an upstream reorder becomes a
 * build error rather than a silent miswire.
 */
BUILD_ASSERT((int)WIFI_SEC_NONE == WIFI_SECURITY_TYPE_NONE &&
	     (int)WIFI_SEC_PSK == WIFI_SECURITY_TYPE_PSK &&
	     (int)WIFI_SEC_PSK_SHA256 == WIFI_SECURITY_TYPE_PSK_SHA256 &&
	     (int)WIFI_SEC_SAE == WIFI_SECURITY_TYPE_SAE,
	     "enum wifi_sec must match Zephyr enum wifi_security_type");

/*
 * Map a Zephyr enum wifi_security_type to our enum wifi_sec. Only the types the
 * connect path supports are distinguished; the rest (WEP, EAP variants, WAPI,
 * DPP, ...) fall through to WIFI_SEC_UNKNOWN and render as "Unknown".
 */
static enum wifi_sec map_security(int sec)
{
	switch (sec) {
	case WIFI_SECURITY_TYPE_NONE:
		return WIFI_SEC_NONE;
	case WIFI_SECURITY_TYPE_WPA_PSK:
	case WIFI_SECURITY_TYPE_PSK:
		return WIFI_SEC_PSK;
	case WIFI_SECURITY_TYPE_PSK_SHA256:
		return WIFI_SEC_PSK_SHA256;
	case WIFI_SECURITY_TYPE_SAE:
		return WIFI_SEC_SAE;
	default:
		return WIFI_SEC_UNKNOWN;
	}
}

static void on_scan_result(const struct wifi_scan_result *r)
{
	k_mutex_lock(&lock, K_FOREVER);
	/*
	 * First-arrival cap, not strongest-N: once the buffer is full further
	 * results are dropped. Fine here because connect is by SSID (never from
	 * this list) and the UI shows only the top few after the RSSI sort.
	 */
	if (ap_count < WIFI_GLUE_MAX_APS) {
		struct wifi_ap *ap = &aps[ap_count];

		wifi_ap_set_ssid(ap, r->ssid, r->ssid_length);
		wifi_ap_set_bssid(ap, r->mac, r->mac_length);
		ap->rssi = r->rssi;
		ap->channel = r->channel;
		ap->security = map_security((int)r->security);
		ap_count++;
	}
	k_mutex_unlock(&lock);
}

static void on_scan_done(const struct wifi_status *st)
{
	int status = (st != NULL) ? st->status : -1;

	size_t n;

	k_mutex_lock(&lock, K_FOREVER);
	ap_count = wifi_scan_dedupe(aps, ap_count);
	wifi_scan_sort(aps, ap_count);
	state = (status == 0) ? WIFI_GLUE_DONE : WIFI_GLUE_ERROR;
	n = ap_count;
	for (size_t i = 0; i < n; i++) {
		snap[i] = aps[i];
	}
	k_mutex_unlock(&lock);

	/*
	 * Log outside the lock, paced, so the ESP32-S3 USB-Serial/JTAG TX ring
	 * buffer is not overwritten mid-burst (a fast 24-line dump loses its first
	 * lines, including the strongest APs).
	 */
	printk("WIFI scan done: status=%d aps=%u\n", status, (unsigned int)n);
	for (size_t i = 0; i < n; i++) {
		printk("WIFI [%u] %-20s ch=%u rssi=%d bars=%u %s\n", (unsigned int)i,
		       snap[i].ssid, (unsigned int)snap[i].channel, (int)snap[i].rssi,
		       (unsigned int)wifi_rssi_bars(snap[i].rssi),
		       wifi_sec_str(snap[i].security));
		k_msleep(5);
	}
}

static void on_connect_result(const struct wifi_status *st)
{
	bool ok = (st != NULL) && (st->status == 0);
	enum wifi_state s;

	k_mutex_lock(&lock, K_FOREVER);
	if (ok) {
		/*
		 * The supplicant owns reconnect and retries indefinitely, so a success
		 * must always land in CONNECTED whatever the mirror's prior state: from
		 * IDLE/RETRY_WAIT this nudges through CONNECTING, from CONNECTING it
		 * completes, from CONNECTED it is a no-op. The glue never drives the
		 * failure-count path (below), so FAILED is unreachable and a late
		 * success is never dropped.
		 */
		wifi_fsm_connect_requested(&conn_fsm);
		wifi_fsm_connect_result(&conn_fsm, true);
	} else {
		/*
		 * A failed attempt or a lost link: drop CONNECTED back to IDLE and
		 * release the lease. We do not count failures (the supplicant, not this
		 * mirror, decides when to give up), so the UI never latches a terminal
		 * state out of sync with a supplicant that keeps retrying.
		 */
		wifi_fsm_disconnected(&conn_fsm);
		have_ip = false;
		ip_str[0] = '\0';
	}
	s = conn_fsm.state;
	k_mutex_unlock(&lock);

	printk("WIFI connect result: status=%d state=%d\n",
	       (st != NULL) ? st->status : -1, (int)s);
}

static void on_disconnect_result(const struct wifi_status *st)
{
	ARG_UNUSED(st);

	k_mutex_lock(&lock, K_FOREVER);
	wifi_fsm_disconnected(&conn_fsm);
	have_ip = false;
	ip_str[0] = '\0';
	k_mutex_unlock(&lock);

	printk("WIFI disconnected\n");
}

static void wifi_event(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
		       struct net_if *iface)
{
	ARG_UNUSED(iface);

	switch (mgmt_event) {
	case NET_EVENT_WIFI_SCAN_RESULT:
		on_scan_result((const struct wifi_scan_result *)cb->info);
		break;
	case NET_EVENT_WIFI_SCAN_DONE:
		on_scan_done((const struct wifi_status *)cb->info);
		break;
	case NET_EVENT_WIFI_CONNECT_RESULT:
		on_connect_result((const struct wifi_status *)cb->info);
		break;
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		on_disconnect_result((const struct wifi_status *)cb->info);
		break;
	default:
		break;
	}
}

static void ipv4_event(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
		       struct net_if *iface)
{
	char buf[NET_IPV4_ADDR_LEN];

	ARG_UNUSED(iface);

	if (mgmt_event != NET_EVENT_IPV4_ADDR_ADD || cb->info == NULL) {
		return;
	}

	net_addr_ntop(AF_INET, cb->info, buf, sizeof(buf));

	k_mutex_lock(&lock, K_FOREVER);
	strcpy(ip_str, buf);
	have_ip = true;
	k_mutex_unlock(&lock);

	/* printk a local copy, not the shared ip_str, to avoid a post-unlock read. */
	printk("WIFI got IPv4 %s\n", buf);
}

int wifi_glue_init(void)
{
	wifi_iface = net_if_get_first_wifi();
	if (wifi_iface == NULL) {
		/*
		 * Single-threaded init, before the callback is registered: no other
		 * thread can observe state yet, so this lone unlocked write is safe.
		 */
		state = WIFI_GLUE_NO_IFACE;
		LOG_ERR("no Wi-Fi interface found");
		return -ENODEV;
	}

	/*
	 * UI-state mirror only: the supplicant owns reconnect, and the glue never
	 * drives the failure-count path (see on_connect_result), so these retry and
	 * backoff numbers are vestigial here and the FAILED/RETRY_WAIT states stay
	 * unreachable in firmware. They keep the pure FSM general and are exercised
	 * by the native_sim tests.
	 */
	wifi_fsm_init(&conn_fsm, 5, 1000, 30000);

	net_mgmt_init_event_callback(&wifi_cb, wifi_event, WIFI_EVENTS);
	net_mgmt_add_event_callback(&wifi_cb);
	net_mgmt_init_event_callback(&ipv4_cb, ipv4_event, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);
	LOG_INF("Wi-Fi glue ready");
	return 0;
}

int wifi_glue_scan_start(void)
{
	int ret;

	if (wifi_iface == NULL) {
		return -ENODEV;
	}

	k_mutex_lock(&lock, K_FOREVER);
	ap_count = 0;
	state = WIFI_GLUE_SCANNING;
	k_mutex_unlock(&lock);

	ret = net_mgmt(NET_REQUEST_WIFI_SCAN, wifi_iface, NULL, 0);
	if (ret) {
		LOG_ERR("scan request failed: %d", ret);
		k_mutex_lock(&lock, K_FOREVER);
		state = WIFI_GLUE_IDLE;
		k_mutex_unlock(&lock);
	} else {
		printk("WIFI scan started\n");
	}
	return ret;
}

enum wifi_glue_state wifi_glue_state(void)
{
	enum wifi_glue_state s;

	k_mutex_lock(&lock, K_FOREVER);
	s = state;
	k_mutex_unlock(&lock);
	return s;
}

size_t wifi_glue_snapshot(struct wifi_ap *out, size_t max)
{
	size_t n;

	if (out == NULL) {
		return 0;
	}

	k_mutex_lock(&lock, K_FOREVER);
	n = MIN(ap_count, max);
	for (size_t i = 0; i < n; i++) {
		out[i] = aps[i];
	}
	k_mutex_unlock(&lock);
	return n;
}

int wifi_glue_connect(const char *ssid, const char *psk, enum wifi_sec sec)
{
	struct wifi_connect_req_params p = {0};
	struct wifi_cfg cfg = {
		.ssid = ssid,
		.ssid_len = (ssid != NULL) ? strlen(ssid) : 0,
		.psk = psk,
		.psk_len = (psk != NULL) ? strlen(psk) : 0,
		.security = sec,
	};
	enum wifi_cfg_status vs;
	int ret;

	if (wifi_iface == NULL) {
		return -ENODEV;
	}

	vs = wifi_cfg_validate(&cfg);
	if (vs != WIFI_CFG_OK) {
		LOG_ERR("invalid Wi-Fi config: %d", (int)vs);
		return -EINVAL;
	}

	/* enum wifi_sec values match Zephyr enum wifi_security_type (see wifi.h). */
	p.ssid = (const uint8_t *)ssid;
	p.ssid_length = (uint8_t)cfg.ssid_len;
	p.security = (enum wifi_security_type)sec;
	if (wifi_sec_needs_psk(sec)) {
		p.psk = (const uint8_t *)psk;
		p.psk_length = (uint8_t)cfg.psk_len;
	}
	p.channel = WIFI_CHANNEL_ANY;
	p.band = WIFI_FREQ_BAND_2_4_GHZ;
	p.mfp = WIFI_MFP_OPTIONAL;

	k_mutex_lock(&lock, K_FOREVER);
	wifi_fsm_connect_requested(&conn_fsm);
	k_mutex_unlock(&lock);

	/* Log the SSID only; never log the passphrase. */
	printk("WIFI connecting to \"%s\"\n", ssid);
	ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, wifi_iface, &p, sizeof(p));
	if (ret) {
		LOG_ERR("connect request failed: %d", ret);
		/* The request never started; return the mirror to IDLE. */
		k_mutex_lock(&lock, K_FOREVER);
		wifi_fsm_reset(&conn_fsm);
		k_mutex_unlock(&lock);
	}
	return ret;
}

enum wifi_state wifi_glue_conn_state(void)
{
	enum wifi_state s;

	k_mutex_lock(&lock, K_FOREVER);
	s = conn_fsm.state;
	k_mutex_unlock(&lock);
	return s;
}

size_t wifi_glue_ipv4(char *out, size_t max)
{
	size_t n = 0;

	if (out == NULL || max == 0) {
		return 0;
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (have_ip) {
		strncpy(out, ip_str, max - 1);
		out[max - 1] = '\0';
		n = strlen(out);
	} else {
		out[0] = '\0';
	}
	k_mutex_unlock(&lock);
	return n;
}
