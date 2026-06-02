/* SPDX-License-Identifier: Apache-2.0 */
#ifndef M5STICKS3_WIFI_GLUE_H
#define M5STICKS3_WIFI_GLUE_H

#ifdef CONFIG_APP_WIFI

#include <stddef.h>

#include "wifi.h"

/*
 * Hardware glue for the Wi-Fi station feature, gated behind CONFIG_APP_WIFI.
 * This is the thin shell over Zephyr's wifi_mgmt / net_mgmt API; the scan-result
 * processing (clamp SSID/BSSID, dedupe, sort) and the connection logic live in
 * the pure, native_sim-tested app/src/wifi.c. wifi_glue.c is only compiled when
 * CONFIG_APP_WIFI is set (see CMakeLists.txt), so the default build is unchanged.
 */

enum wifi_glue_state {
	WIFI_GLUE_IDLE = 0,  /* no scan run yet */
	WIFI_GLUE_SCANNING,  /* a scan is in progress */
	WIFI_GLUE_DONE,      /* the last scan finished; results are ready */
	WIFI_GLUE_ERROR,     /* the last scan completed with a nonzero status */
	WIFI_GLUE_NO_IFACE,  /* no Wi-Fi interface was found at init */
};

/* Find the Wi-Fi interface and register the scan event callbacks. 0 on success. */
int wifi_glue_init(void);

/* Start an asynchronous scan. Results arrive via callbacks; state -> SCANNING. */
int wifi_glue_scan_start(void);

/* Current scan state, for the UI. */
enum wifi_glue_state wifi_glue_state(void);

/*
 * Copy up to max deduplicated, RSSI-sorted scan results into out; returns the
 * number copied. Safe to call from the UI thread while a scan runs (the
 * snapshot is taken under a lock).
 */
size_t wifi_glue_snapshot(struct wifi_ap *out, size_t max);

/*
 * Request a station connection to ssid using psk (WPA2-PSK; pass WIFI_SEC_NONE
 * with a NULL/empty psk for an open network). The config is validated first;
 * the call is asynchronous and the outcome arrives via the connection event.
 * Returns 0 if the request was accepted, negative otherwise. The psk is never
 * logged.
 */
int wifi_glue_connect(const char *ssid, const char *psk, enum wifi_sec sec);

/* Current connection state (the UI-facing FSM mirror). */
enum wifi_state wifi_glue_conn_state(void);

/*
 * Copy the DHCP-assigned IPv4 address (dotted-quad, NUL-terminated) into out and
 * return its length, or 0 with out[0] = '\0' if there is no lease yet. out must
 * hold at least 16 bytes.
 */
size_t wifi_glue_ipv4(char *out, size_t max);

#endif /* CONFIG_APP_WIFI */

#endif /* M5STICKS3_WIFI_GLUE_H */
