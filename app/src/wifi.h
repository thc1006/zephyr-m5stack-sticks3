/* SPDX-License-Identifier: Apache-2.0 */
#ifndef M5STICKS3_WIFI_H
#define M5STICKS3_WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Wi-Fi station logic as pure functions, unit-testable on native_sim without
 * the ESP32 Wi-Fi driver or any RF hardware. The net_mgmt / wifi_mgmt glue
 * (scan + connect requests and the event callbacks) lives in app/src/wifi_glue.c
 * and is a thin shell that feeds these functions. This mirrors the nec.c (pure)
 * under ir.c (hardware) split already used in this repo.
 *
 * Security values match Zephyr enum wifi_security_type so the glue passes them
 * through without translation (NONE=0, PSK=1, PSK_SHA256=2, SAE=3).
 */

/* SSID and PSK length limits (IEEE 802.11 / WPA2-PSK). */
#define M5WIFI_SSID_MIN_LEN 1U
#define M5WIFI_SSID_MAX_LEN 32U
#define M5WIFI_PSK_MIN_LEN     8U
#define M5WIFI_PSK_MAX_LEN     63U  /* WPA2-PSK ASCII passphrase max */
#define M5WIFI_SAE_PSK_MAX_LEN 128U /* WPA3-SAE password max */
#define M5WIFI_BSSID_LEN       6U

/* Security types. */
enum wifi_sec {
	WIFI_SEC_NONE = 0,       /* open */
	WIFI_SEC_PSK = 1,        /* WPA2-PSK */
	WIFI_SEC_PSK_SHA256 = 2,
	WIFI_SEC_SAE = 3,        /* WPA3-SAE */
	WIFI_SEC_UNKNOWN = 0xFF, /* our sentinel, not a Zephyr value */
};

/* Result of validating a station config; OK or the first failing reason. */
enum wifi_cfg_status {
	WIFI_CFG_OK = 0,
	WIFI_CFG_SSID_EMPTY = -1,
	WIFI_CFG_SSID_TOO_LONG = -2,
	WIFI_CFG_PSK_TOO_SHORT = -3,
	WIFI_CFG_PSK_TOO_LONG = -4,
	WIFI_CFG_SECURITY_INVALID = -5,
	WIFI_CFG_PSK_MISSING = -6, /* key-based security but psk is NULL */
};

struct wifi_cfg {
	const char *ssid;
	size_t ssid_len;
	const char *psk;
	size_t psk_len;
	enum wifi_sec security;
};

/*
 * Validate a station config before handing it to the driver. Checks in order:
 * SSID length 1..32; security type known; then, for a key-based security, the
 * key. WPA2-PSK and PSK-SHA256 take an 8..63 byte PSK; WPA3-SAE takes an 8..128
 * byte password; a key-based security with a NULL psk is rejected
 * (WIFI_CFG_PSK_MISSING). Open security ignores the key. A NULL cfg is rejected
 * as WIFI_CFG_SECURITY_INVALID. ssid_len/psk_len are trusted to match their
 * buffers (this checks the lengths, not the allocations). Returns WIFI_CFG_OK or
 * the first failing reason.
 */
enum wifi_cfg_status wifi_cfg_validate(const struct wifi_cfg *cfg);

/* True if the security type uses a pre-shared key (so a PSK is required). */
bool wifi_sec_needs_psk(enum wifi_sec sec);

/*
 * Human-readable security label: "Open", "WPA2-PSK", "WPA2-PSK-256", "WPA3-SAE",
 * or "Unknown". Never NULL.
 */
const char *wifi_sec_str(enum wifi_sec sec);

/*
 * Map an RSSI in dBm to a 0..4 signal-bar count for the LCD:
 * >= -55 => 4, >= -65 => 3, >= -75 => 2, >= -85 => 1, else 0.
 */
uint8_t wifi_rssi_bars(int8_t rssi_dbm);

/*
 * One scan result. Zephyr's struct wifi_scan_result carries the BSSID as
 * mac[6] + mac_length and the SSID as ssid[33] + ssid_length; the glue maps
 * those in via wifi_ap_set_bssid() and wifi_ap_set_ssid() (never raw copies).
 */
struct wifi_ap {
	uint8_t bssid[M5WIFI_BSSID_LEN];
	char ssid[M5WIFI_SSID_MAX_LEN + 1]; /* NUL-terminated */
	int8_t rssi;
	uint8_t channel;
	enum wifi_sec security;
};

/*
 * Copy a scan-result SSID into ap->ssid safely: clamps src to M5WIFI_SSID_MAX_LEN
 * bytes and always NUL-terminates. SSID bytes and length come from the RF
 * environment (untrusted), so the glue MUST use this to fill ap->ssid instead of
 * copying directly. A NULL src yields an empty SSID; a NULL ap is a no-op.
 */
void wifi_ap_set_ssid(struct wifi_ap *ap, const uint8_t *src, size_t len);

/*
 * Copy a scan-result BSSID (Zephyr's mac/mac_length) into ap->bssid. A full
 * 6-byte MAC is copied; any other length (including a missing MAC) zero-fills
 * the BSSID, so results with no MAC share an all-zero BSSID and may collapse in
 * wifi_scan_dedupe (the glue can skip mac_length == 0 if that matters). A NULL
 * ap is a no-op.
 */
void wifi_ap_set_bssid(struct wifi_ap *ap, const uint8_t *mac, size_t mac_len);

/*
 * Deduplicate scan results in place by BSSID, keeping the strongest-RSSI entry
 * for each BSSID. Returns the new count. (Callers sort by RSSI next, so the
 * relative order of survivors is unspecified.) Pipeline per result:
 * wifi_ap_set_ssid + wifi_ap_set_bssid, then wifi_scan_dedupe, then wifi_scan_sort.
 */
size_t wifi_scan_dedupe(struct wifi_ap *aps, size_t count);

/* Sort scan results in place by RSSI, strongest (least negative) first. */
void wifi_scan_sort(struct wifi_ap *aps, size_t count);

/*
 * Connection state machine (pure; no kernel timers, caller drives the waits).
 * backoff_ms and wifi_fsm_should_retry() are advisory: reconnect must be driven
 * by exactly one owner. For the StickS3 station demo the WPA supplicant owns
 * reconnect and this FSM tracks the UI state only; if instead the FSM owns
 * retries, the supplicant auto-reconnect must be disabled.
 */
enum wifi_state {
	M5WIFI_STATE_IDLE = 0,
	M5WIFI_STATE_CONNECTING,
	M5WIFI_STATE_CONNECTED,
	M5WIFI_STATE_RETRY_WAIT,
	M5WIFI_STATE_FAILED,
};

struct wifi_fsm {
	enum wifi_state state;
	uint8_t attempts;     /* connect attempts made since last success */
	uint8_t max_attempts; /* give up after this many failures */
	uint32_t backoff_ms;  /* wait before the next retry */
	uint32_t base_backoff_ms;
	uint32_t max_backoff_ms;
};

/*
 * Initialise the FSM in IDLE with the given retry policy. If max_backoff_ms is
 * less than base_backoff_ms it is raised to base_backoff_ms.
 */
void wifi_fsm_init(struct wifi_fsm *f, uint8_t max_attempts,
		   uint32_t base_backoff_ms, uint32_t max_backoff_ms);

/* A connect was requested: IDLE/RETRY_WAIT -> CONNECTING, counts one attempt. */
void wifi_fsm_connect_requested(struct wifi_fsm *f);

/*
 * A connect result arrived. ok=true -> CONNECTED, attempts and backoff reset.
 * ok=false -> RETRY_WAIT with backoff doubled (capped at max) if attempts
 * remain, else FAILED.
 */
void wifi_fsm_connect_result(struct wifi_fsm *f, bool ok);

/* A disconnect happened while CONNECTED -> IDLE (ready to reconnect). */
void wifi_fsm_disconnected(struct wifi_fsm *f);

/*
 * Reset to IDLE from any state (e.g. a UI "retry" after FAILED), clearing the
 * attempt counter and backoff while keeping the retry policy. FAILED is
 * otherwise terminal.
 */
void wifi_fsm_reset(struct wifi_fsm *f);

/* True if the FSM is waiting to retry (attempts remain). */
bool wifi_fsm_should_retry(const struct wifi_fsm *f);

#endif /* M5STICKS3_WIFI_H */
