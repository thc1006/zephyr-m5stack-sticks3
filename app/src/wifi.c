/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Pure Wi-Fi station logic (see wifi.h). No hardware, no Zephyr net headers, so
 * this builds and is unit-tested on native_sim. The wifi_mgmt / net_mgmt glue
 * in wifi_glue.c feeds these functions.
 */

#include <stdbool.h>
#include <string.h>

#include "wifi.h"

enum wifi_cfg_status wifi_cfg_validate(const struct wifi_cfg *cfg)
{
	if (cfg == NULL) {
		return WIFI_CFG_SECURITY_INVALID;
	}

	/* SSID length 1..32. */
	if (cfg->ssid == NULL || cfg->ssid_len < M5WIFI_SSID_MIN_LEN) {
		return WIFI_CFG_SSID_EMPTY;
	}
	if (cfg->ssid_len > M5WIFI_SSID_MAX_LEN) {
		return WIFI_CFG_SSID_TOO_LONG;
	}

	/* Security type must be known. */
	switch (cfg->security) {
	case WIFI_SEC_NONE:
	case WIFI_SEC_PSK:
	case WIFI_SEC_PSK_SHA256:
	case WIFI_SEC_SAE:
		break;
	default:
		return WIFI_CFG_SECURITY_INVALID;
	}

	/* Open security ignores the key. */
	if (!wifi_sec_needs_psk(cfg->security)) {
		return WIFI_CFG_OK;
	}

	/* Key-based security needs a present key of the right length. */
	if (cfg->psk == NULL) {
		return WIFI_CFG_PSK_MISSING;
	}
	if (cfg->psk_len < M5WIFI_PSK_MIN_LEN) {
		return WIFI_CFG_PSK_TOO_SHORT;
	}
	if (cfg->psk_len > ((cfg->security == WIFI_SEC_SAE) ? M5WIFI_SAE_PSK_MAX_LEN
							    : M5WIFI_PSK_MAX_LEN)) {
		return WIFI_CFG_PSK_TOO_LONG;
	}

	return WIFI_CFG_OK;
}

bool wifi_sec_needs_psk(enum wifi_sec sec)
{
	return sec == WIFI_SEC_PSK || sec == WIFI_SEC_PSK_SHA256 ||
	       sec == WIFI_SEC_SAE;
}

const char *wifi_sec_str(enum wifi_sec sec)
{
	switch (sec) {
	case WIFI_SEC_NONE:
		return "Open";
	case WIFI_SEC_PSK:
		return "WPA2-PSK";
	case WIFI_SEC_PSK_SHA256:
		return "WPA2-PSK-256";
	case WIFI_SEC_SAE:
		return "WPA3-SAE";
	default:
		return "Unknown";
	}
}

uint8_t wifi_rssi_bars(int8_t rssi_dbm)
{
	if (rssi_dbm >= -55) {
		return 4;
	}
	if (rssi_dbm >= -65) {
		return 3;
	}
	if (rssi_dbm >= -75) {
		return 2;
	}
	if (rssi_dbm >= -85) {
		return 1;
	}
	return 0;
}

void wifi_ap_set_ssid(struct wifi_ap *ap, const uint8_t *src, size_t len)
{
	if (ap == NULL) {
		return;
	}
	if (src == NULL) {
		len = 0;
	}
	if (len > M5WIFI_SSID_MAX_LEN) {
		len = M5WIFI_SSID_MAX_LEN;
	}
	if (len > 0) {
		memcpy(ap->ssid, src, len);
	}
	ap->ssid[len] = '\0';
}

void wifi_ap_set_bssid(struct wifi_ap *ap, const uint8_t *mac, size_t mac_len)
{
	if (ap == NULL) {
		return;
	}
	if (mac != NULL && mac_len == M5WIFI_BSSID_LEN) {
		memcpy(ap->bssid, mac, M5WIFI_BSSID_LEN);
	} else {
		memset(ap->bssid, 0, M5WIFI_BSSID_LEN);
	}
}

size_t wifi_scan_dedupe(struct wifi_ap *aps, size_t count)
{
	size_t out = 0;

	if (aps == NULL) {
		return 0;
	}

	for (size_t i = 0; i < count; i++) {
		size_t j;

		for (j = 0; j < out; j++) {
			if (memcmp(aps[j].bssid, aps[i].bssid, M5WIFI_BSSID_LEN) == 0) {
				break;
			}
		}

		if (j < out) {
			/* Same BSSID already kept: replace if this one is stronger. */
			if (aps[i].rssi > aps[j].rssi) {
				aps[j] = aps[i];
			}
		} else {
			aps[out++] = aps[i];
		}
	}

	return out;
}

void wifi_scan_sort(struct wifi_ap *aps, size_t count)
{
	if (aps == NULL) {
		return;
	}

	/* Insertion sort by RSSI descending; scan lists are short. */
	for (size_t i = 1; i < count; i++) {
		struct wifi_ap key = aps[i];
		size_t j = i;

		while (j > 0 && aps[j - 1].rssi < key.rssi) {
			aps[j] = aps[j - 1];
			j--;
		}
		aps[j] = key;
	}
}

void wifi_fsm_init(struct wifi_fsm *f, uint8_t max_attempts,
		   uint32_t base_backoff_ms, uint32_t max_backoff_ms)
{
	if (f == NULL) {
		return;
	}
	if (max_backoff_ms < base_backoff_ms) {
		max_backoff_ms = base_backoff_ms;
	}
	f->state = M5WIFI_STATE_IDLE;
	f->attempts = 0;
	f->max_attempts = max_attempts;
	f->base_backoff_ms = base_backoff_ms;
	f->max_backoff_ms = max_backoff_ms;
	f->backoff_ms = base_backoff_ms;
}

void wifi_fsm_connect_requested(struct wifi_fsm *f)
{
	if (f == NULL) {
		return;
	}
	if (f->state == M5WIFI_STATE_IDLE || f->state == M5WIFI_STATE_RETRY_WAIT) {
		f->state = M5WIFI_STATE_CONNECTING;
		f->attempts++;
	}
}

void wifi_fsm_connect_result(struct wifi_fsm *f, bool ok)
{
	if (f == NULL || f->state != M5WIFI_STATE_CONNECTING) {
		return;
	}

	if (ok) {
		f->state = M5WIFI_STATE_CONNECTED;
		f->attempts = 0;
		f->backoff_ms = f->base_backoff_ms;
		return;
	}

	if (f->attempts < f->max_attempts) {
		uint32_t next = f->backoff_ms * 2U;

		/* Cap at max, and guard against uint32 overflow on doubling. */
		if (next > f->max_backoff_ms || next < f->backoff_ms) {
			next = f->max_backoff_ms;
		}
		f->backoff_ms = next;
		f->state = M5WIFI_STATE_RETRY_WAIT;
	} else {
		f->state = M5WIFI_STATE_FAILED;
	}
}

void wifi_fsm_disconnected(struct wifi_fsm *f)
{
	if (f == NULL) {
		return;
	}
	if (f->state == M5WIFI_STATE_CONNECTED) {
		f->state = M5WIFI_STATE_IDLE;
	}
}

void wifi_fsm_reset(struct wifi_fsm *f)
{
	if (f == NULL) {
		return;
	}
	f->state = M5WIFI_STATE_IDLE;
	f->attempts = 0;
	f->backoff_ms = f->base_backoff_ms;
}

bool wifi_fsm_should_retry(const struct wifi_fsm *f)
{
	return f != NULL && f->state == M5WIFI_STATE_RETRY_WAIT;
}
