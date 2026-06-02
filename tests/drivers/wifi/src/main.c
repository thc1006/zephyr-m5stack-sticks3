/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>

#include <zephyr/ztest.h>

#include "wifi.h"

/* ------------------------------------------------------------------ */
/* Config validation                                                   */
/* ------------------------------------------------------------------ */

ZTEST_SUITE(wifi_cfg, NULL, NULL, NULL, NULL, NULL);

static struct wifi_cfg cfg_psk(const char *ssid, size_t ssid_len,
			       const char *psk, size_t psk_len)
{
	struct wifi_cfg c = {
		.ssid = ssid,
		.ssid_len = ssid_len,
		.psk = psk,
		.psk_len = psk_len,
		.security = WIFI_SEC_PSK,
	};
	return c;
}

ZTEST(wifi_cfg, test_valid_wpa2)
{
	struct wifi_cfg c = cfg_psk("HomeNet", 7, "password1", 9);

	zassert_equal(wifi_cfg_validate(&c), WIFI_CFG_OK);
}

ZTEST(wifi_cfg, test_ssid_empty)
{
	struct wifi_cfg c = cfg_psk("", 0, "password1", 9);

	zassert_equal(wifi_cfg_validate(&c), WIFI_CFG_SSID_EMPTY);
}

ZTEST(wifi_cfg, test_ssid_max_ok)
{
	char ssid[M5WIFI_SSID_MAX_LEN];

	memset(ssid, 'X', sizeof(ssid));
	struct wifi_cfg c = cfg_psk(ssid, M5WIFI_SSID_MAX_LEN, "password1", 9);

	zassert_equal(wifi_cfg_validate(&c), WIFI_CFG_OK);
}

ZTEST(wifi_cfg, test_ssid_too_long)
{
	char ssid[M5WIFI_SSID_MAX_LEN + 1];

	memset(ssid, 'X', sizeof(ssid));
	struct wifi_cfg c = cfg_psk(ssid, M5WIFI_SSID_MAX_LEN + 1, "password1", 9);

	zassert_equal(wifi_cfg_validate(&c), WIFI_CFG_SSID_TOO_LONG);
}

ZTEST(wifi_cfg, test_psk_too_short)
{
	struct wifi_cfg c = cfg_psk("HomeNet", 7, "short", 5);

	zassert_equal(wifi_cfg_validate(&c), WIFI_CFG_PSK_TOO_SHORT);
}

ZTEST(wifi_cfg, test_psk_min_ok)
{
	struct wifi_cfg c = cfg_psk("HomeNet", 7, "12345678", M5WIFI_PSK_MIN_LEN);

	zassert_equal(wifi_cfg_validate(&c), WIFI_CFG_OK);
}

ZTEST(wifi_cfg, test_psk_too_long)
{
	char psk[M5WIFI_PSK_MAX_LEN + 1];

	memset(psk, 'p', sizeof(psk));
	struct wifi_cfg c = cfg_psk("HomeNet", 7, psk, M5WIFI_PSK_MAX_LEN + 1);

	zassert_equal(wifi_cfg_validate(&c), WIFI_CFG_PSK_TOO_LONG);
}

ZTEST(wifi_cfg, test_open_ignores_psk)
{
	struct wifi_cfg c = {
		.ssid = "OpenNet",
		.ssid_len = 7,
		.psk = NULL,
		.psk_len = 0,
		.security = WIFI_SEC_NONE,
	};

	zassert_equal(wifi_cfg_validate(&c), WIFI_CFG_OK);
}

ZTEST(wifi_cfg, test_unknown_security)
{
	struct wifi_cfg c = {
		.ssid = "HomeNet",
		.ssid_len = 7,
		.psk = "password1",
		.psk_len = 9,
		.security = WIFI_SEC_UNKNOWN,
	};

	zassert_equal(wifi_cfg_validate(&c), WIFI_CFG_SECURITY_INVALID);
}

ZTEST(wifi_cfg, test_sae_long_password_ok)
{
	char psk[80];

	memset(psk, 'p', sizeof(psk));
	struct wifi_cfg c = {
		.ssid = "HomeNet",
		.ssid_len = 7,
		.psk = psk,
		.psk_len = sizeof(psk),
		.security = WIFI_SEC_SAE,
	};

	zassert_equal(wifi_cfg_validate(&c), WIFI_CFG_OK, "SAE allows >63 byte password");
}

ZTEST(wifi_cfg, test_sae_password_too_long)
{
	char psk[M5WIFI_SAE_PSK_MAX_LEN + 1];

	memset(psk, 'p', sizeof(psk));
	struct wifi_cfg c = {
		.ssid = "HomeNet",
		.ssid_len = 7,
		.psk = psk,
		.psk_len = M5WIFI_SAE_PSK_MAX_LEN + 1,
		.security = WIFI_SEC_SAE,
	};

	zassert_equal(wifi_cfg_validate(&c), WIFI_CFG_PSK_TOO_LONG);
}

ZTEST(wifi_cfg, test_psk_null_missing)
{
	struct wifi_cfg c = {
		.ssid = "HomeNet",
		.ssid_len = 7,
		.psk = NULL,
		.psk_len = 10,
		.security = WIFI_SEC_PSK,
	};

	zassert_equal(wifi_cfg_validate(&c), WIFI_CFG_PSK_MISSING);
}

ZTEST(wifi_cfg, test_psk_sha256_uses_wpa2_limit)
{
	char psk[M5WIFI_PSK_MAX_LEN + 1];

	memset(psk, 'p', sizeof(psk));
	struct wifi_cfg c = {
		.ssid = "HomeNet",
		.ssid_len = 7,
		.psk = psk,
		.psk_len = M5WIFI_PSK_MAX_LEN + 1,
		.security = WIFI_SEC_PSK_SHA256,
	};

	zassert_equal(wifi_cfg_validate(&c), WIFI_CFG_PSK_TOO_LONG, "PSK-SHA256 capped at 63");
}

/* ------------------------------------------------------------------ */
/* Security helpers                                                    */
/* ------------------------------------------------------------------ */

ZTEST_SUITE(wifi_sec, NULL, NULL, NULL, NULL, NULL);

ZTEST(wifi_sec, test_needs_psk)
{
	zassert_false(wifi_sec_needs_psk(WIFI_SEC_NONE));
	zassert_true(wifi_sec_needs_psk(WIFI_SEC_PSK));
	zassert_true(wifi_sec_needs_psk(WIFI_SEC_SAE));
}

ZTEST(wifi_sec, test_str_never_null)
{
	zassert_not_null(wifi_sec_str(WIFI_SEC_NONE));
	zassert_not_null(wifi_sec_str(WIFI_SEC_PSK));
	zassert_not_null(wifi_sec_str(WIFI_SEC_SAE));
	zassert_not_null(wifi_sec_str(WIFI_SEC_UNKNOWN));
}

ZTEST(wifi_sec, test_str_values)
{
	zassert_true(strcmp(wifi_sec_str(WIFI_SEC_NONE), "Open") == 0);
	zassert_true(strcmp(wifi_sec_str(WIFI_SEC_PSK), "WPA2-PSK") == 0);
	zassert_true(strcmp(wifi_sec_str(WIFI_SEC_SAE), "WPA3-SAE") == 0);
}

/* ------------------------------------------------------------------ */
/* RSSI to signal bars                                                 */
/* ------------------------------------------------------------------ */

ZTEST_SUITE(wifi_rssi, NULL, NULL, NULL, NULL, NULL);

ZTEST(wifi_rssi, test_bars_thresholds)
{
	zassert_equal(wifi_rssi_bars(-40), 4);
	zassert_equal(wifi_rssi_bars(-55), 4, ">= -55 is 4 bars");
	zassert_equal(wifi_rssi_bars(-56), 3);
	zassert_equal(wifi_rssi_bars(-65), 3, ">= -65 is 3 bars");
	zassert_equal(wifi_rssi_bars(-66), 2);
	zassert_equal(wifi_rssi_bars(-75), 2, ">= -75 is 2 bars");
	zassert_equal(wifi_rssi_bars(-76), 1);
	zassert_equal(wifi_rssi_bars(-85), 1, ">= -85 is 1 bar");
	zassert_equal(wifi_rssi_bars(-86), 0);
}

/* ------------------------------------------------------------------ */
/* Scan list: dedupe + sort                                            */
/* ------------------------------------------------------------------ */

ZTEST_SUITE(wifi_scan, NULL, NULL, NULL, NULL, NULL);

ZTEST(wifi_scan, test_dedupe_keeps_strongest)
{
	struct wifi_ap aps[3] = {
		{ .bssid = {1, 2, 3, 4, 5, 6}, .ssid = "A", .rssi = -70 },
		{ .bssid = {1, 2, 3, 4, 5, 6}, .ssid = "A", .rssi = -50 },
		{ .bssid = {9, 9, 9, 9, 9, 9}, .ssid = "B", .rssi = -60 },
	};

	size_t n = wifi_scan_dedupe(aps, 3);

	zassert_equal(n, 2, "two unique BSSIDs");
	/* The surviving "A" entry must carry the stronger -50 RSSI. */
	bool found_a_strong = false;

	for (size_t i = 0; i < n; i++) {
		if (strcmp(aps[i].ssid, "A") == 0) {
			zassert_equal(aps[i].rssi, -50, "kept strongest A");
			found_a_strong = true;
		}
	}
	zassert_true(found_a_strong, "A survived");
}

ZTEST(wifi_scan, test_sort_by_rssi_desc)
{
	struct wifi_ap aps[3] = {
		{ .rssi = -70 },
		{ .rssi = -30 },
		{ .rssi = -50 },
	};

	wifi_scan_sort(aps, 3);

	zassert_equal(aps[0].rssi, -30, "strongest first");
	zassert_equal(aps[1].rssi, -50);
	zassert_equal(aps[2].rssi, -70, "weakest last");
}

/* ------------------------------------------------------------------ */
/* Connection state machine                                            */
/* ------------------------------------------------------------------ */

ZTEST_SUITE(wifi_fsm, NULL, NULL, NULL, NULL, NULL);

ZTEST(wifi_fsm, test_init)
{
	struct wifi_fsm f;

	wifi_fsm_init(&f, 4, 100, 800);
	zassert_equal(f.state, M5WIFI_STATE_IDLE);
	zassert_equal(f.attempts, 0);
	zassert_equal(f.backoff_ms, 100);
}

ZTEST(wifi_fsm, test_connect_success)
{
	struct wifi_fsm f;

	wifi_fsm_init(&f, 4, 100, 800);
	wifi_fsm_connect_requested(&f);
	zassert_equal(f.state, M5WIFI_STATE_CONNECTING);
	zassert_equal(f.attempts, 1);

	wifi_fsm_connect_result(&f, true);
	zassert_equal(f.state, M5WIFI_STATE_CONNECTED);
	zassert_equal(f.attempts, 0, "attempts reset on success");
}

ZTEST(wifi_fsm, test_success_resets_backoff)
{
	struct wifi_fsm f;

	wifi_fsm_init(&f, 4, 100, 800);
	wifi_fsm_connect_requested(&f);
	wifi_fsm_connect_result(&f, false); /* backoff 100 -> 200 */
	zassert_equal(f.backoff_ms, 200);

	wifi_fsm_connect_requested(&f);
	wifi_fsm_connect_result(&f, true); /* success resets */
	zassert_equal(f.state, M5WIFI_STATE_CONNECTED);
	zassert_equal(f.backoff_ms, 100, "backoff reset to base on success");
}

ZTEST(wifi_fsm, test_backoff_doubles_and_caps)
{
	struct wifi_fsm f;

	wifi_fsm_init(&f, 5, 100, 400);

	wifi_fsm_connect_requested(&f);
	wifi_fsm_connect_result(&f, false);
	zassert_equal(f.state, M5WIFI_STATE_RETRY_WAIT);
	zassert_equal(f.backoff_ms, 200, "100 -> 200");
	zassert_true(wifi_fsm_should_retry(&f));

	wifi_fsm_connect_requested(&f);
	wifi_fsm_connect_result(&f, false);
	zassert_equal(f.backoff_ms, 400, "200 -> 400");

	wifi_fsm_connect_requested(&f);
	wifi_fsm_connect_result(&f, false);
	zassert_equal(f.backoff_ms, 400, "capped at max 400");
}

ZTEST(wifi_fsm, test_gives_up_after_max)
{
	struct wifi_fsm f;

	wifi_fsm_init(&f, 3, 100, 800);

	for (int i = 0; i < 3; i++) {
		wifi_fsm_connect_requested(&f);
		wifi_fsm_connect_result(&f, false);
	}

	zassert_equal(f.state, M5WIFI_STATE_FAILED, "gave up after 3");
	zassert_false(wifi_fsm_should_retry(&f));
}

ZTEST(wifi_fsm, test_disconnect_returns_idle)
{
	struct wifi_fsm f;

	wifi_fsm_init(&f, 4, 100, 800);
	wifi_fsm_connect_requested(&f);
	wifi_fsm_connect_result(&f, true);
	zassert_equal(f.state, M5WIFI_STATE_CONNECTED);

	wifi_fsm_disconnected(&f);
	zassert_equal(f.state, M5WIFI_STATE_IDLE);
}

ZTEST(wifi_fsm, test_init_stores_all_fields)
{
	struct wifi_fsm f;

	wifi_fsm_init(&f, 7, 250, 16000);
	zassert_equal(f.state, M5WIFI_STATE_IDLE);
	zassert_equal(f.attempts, 0);
	zassert_equal(f.max_attempts, 7);
	zassert_equal(f.base_backoff_ms, 250);
	zassert_equal(f.max_backoff_ms, 16000);
	zassert_equal(f.backoff_ms, 250);
}

ZTEST(wifi_fsm, test_init_clamps_max_below_base)
{
	struct wifi_fsm f;

	wifi_fsm_init(&f, 4, 1000, 100);
	zassert_equal(f.max_backoff_ms, 1000, "max raised to base when smaller");
}

ZTEST(wifi_fsm, test_spurious_result_from_idle)
{
	struct wifi_fsm f;

	wifi_fsm_init(&f, 4, 100, 800);
	wifi_fsm_connect_result(&f, true);
	zassert_equal(f.state, M5WIFI_STATE_IDLE, "result in IDLE is a no-op");
	zassert_equal(f.attempts, 0);
}

ZTEST(wifi_fsm, test_spurious_result_from_connected)
{
	struct wifi_fsm f;

	wifi_fsm_init(&f, 4, 100, 800);
	wifi_fsm_connect_requested(&f);
	wifi_fsm_connect_result(&f, true);
	wifi_fsm_connect_result(&f, false);
	zassert_equal(f.state, M5WIFI_STATE_CONNECTED, "stray failure ignored");
	zassert_equal(f.backoff_ms, 100);
}

ZTEST(wifi_fsm, test_spurious_request_from_connected)
{
	struct wifi_fsm f;

	wifi_fsm_init(&f, 4, 100, 800);
	wifi_fsm_connect_requested(&f);
	wifi_fsm_connect_result(&f, true);
	wifi_fsm_connect_requested(&f);
	zassert_equal(f.state, M5WIFI_STATE_CONNECTED);
	zassert_equal(f.attempts, 0, "no extra attempt while connected");
}

ZTEST(wifi_fsm, test_spurious_disconnect_from_connecting)
{
	struct wifi_fsm f;

	wifi_fsm_init(&f, 4, 100, 800);
	wifi_fsm_connect_requested(&f);
	wifi_fsm_disconnected(&f);
	zassert_equal(f.state, M5WIFI_STATE_CONNECTING, "disconnect ignored mid-connect");
}

ZTEST(wifi_fsm, test_spurious_disconnect_from_retry_wait)
{
	struct wifi_fsm f;

	wifi_fsm_init(&f, 4, 100, 800);
	wifi_fsm_connect_requested(&f);
	wifi_fsm_connect_result(&f, false);
	wifi_fsm_disconnected(&f);
	zassert_equal(f.state, M5WIFI_STATE_RETRY_WAIT, "disconnect ignored in retry-wait");
}

ZTEST(wifi_fsm, test_backoff_overflow_guard)
{
	struct wifi_fsm f;

	wifi_fsm_init(&f, 5, 0xC0000000U, 0xFFFFFFFFU);
	wifi_fsm_connect_requested(&f);
	wifi_fsm_connect_result(&f, false);
	zassert_equal(f.state, M5WIFI_STATE_RETRY_WAIT);
	zassert_equal(f.backoff_ms, 0xFFFFFFFFU, "doubling overflow clamps to max");
}

ZTEST(wifi_cfg, test_null_cfg)
{
	zassert_equal(wifi_cfg_validate(NULL), WIFI_CFG_SECURITY_INVALID);
}

ZTEST(wifi_cfg, test_ssid_checked_before_security)
{
	struct wifi_cfg c = {
		.ssid = "",
		.ssid_len = 0,
		.psk = "password1",
		.psk_len = 9,
		.security = WIFI_SEC_UNKNOWN,
	};

	zassert_equal(wifi_cfg_validate(&c), WIFI_CFG_SSID_EMPTY, "SSID precedes security");
}

ZTEST(wifi_cfg, test_security_checked_before_psk)
{
	struct wifi_cfg c = {
		.ssid = "HomeNet",
		.ssid_len = 7,
		.psk = NULL,
		.psk_len = 0,
		.security = WIFI_SEC_UNKNOWN,
	};

	zassert_equal(wifi_cfg_validate(&c), WIFI_CFG_SECURITY_INVALID, "security precedes key");
}

ZTEST(wifi_sec, test_needs_psk_sha256)
{
	zassert_true(wifi_sec_needs_psk(WIFI_SEC_PSK_SHA256));
}

ZTEST(wifi_sec, test_str_psk_sha256_value)
{
	zassert_true(strcmp(wifi_sec_str(WIFI_SEC_PSK_SHA256), "WPA2-PSK-256") == 0);
}

ZTEST(wifi_scan, test_dedupe_empty)
{
	zassert_equal(wifi_scan_dedupe(NULL, 0), 0);
}

ZTEST(wifi_scan, test_dedupe_single)
{
	struct wifi_ap ap = {.bssid = {0xAA, 0, 0, 0, 0, 0}, .rssi = -60};

	zassert_equal(wifi_scan_dedupe(&ap, 1), 1);
	zassert_equal(ap.rssi, -60);
}

ZTEST(wifi_scan, test_dedupe_all_same_bssid)
{
	struct wifi_ap aps[4] = {
		{.bssid = {1, 2, 3, 4, 5, 6}, .rssi = -80},
		{.bssid = {1, 2, 3, 4, 5, 6}, .rssi = -90},
		{.bssid = {1, 2, 3, 4, 5, 6}, .rssi = -40},
		{.bssid = {1, 2, 3, 4, 5, 6}, .rssi = -70},
	};

	zassert_equal(wifi_scan_dedupe(aps, 4), 1, "all same BSSID collapse to one");
	zassert_equal(aps[0].rssi, -40, "strongest survives");
}

ZTEST(wifi_scan, test_sort_empty_and_single)
{
	struct wifi_ap ap = {.rssi = -55};

	wifi_scan_sort(NULL, 0);
	wifi_scan_sort(&ap, 1);
	zassert_equal(ap.rssi, -55);
}

ZTEST(wifi_scan, test_sort_already_sorted)
{
	struct wifi_ap aps[3] = {{.rssi = -30}, {.rssi = -50}, {.rssi = -70}};

	wifi_scan_sort(aps, 3);
	zassert_equal(aps[0].rssi, -30);
	zassert_equal(aps[2].rssi, -70);
}

ZTEST(wifi_scan, test_sort_reverse)
{
	struct wifi_ap aps[3] = {{.rssi = -70}, {.rssi = -50}, {.rssi = -30}};

	wifi_scan_sort(aps, 3);
	zassert_equal(aps[0].rssi, -30);
	zassert_equal(aps[1].rssi, -50);
	zassert_equal(aps[2].rssi, -70);
}

/* ------------------------------------------------------------------ */
/* SSID copy helper (untrusted RF bytes -> fixed buffer)               */
/* ------------------------------------------------------------------ */

ZTEST_SUITE(wifi_ap, NULL, NULL, NULL, NULL, NULL);

ZTEST(wifi_ap, test_set_ssid_normal)
{
	struct wifi_ap ap;

	wifi_ap_set_ssid(&ap, (const uint8_t *)"Net", 3);
	zassert_equal(ap.ssid[3], '\0');
	zassert_true(strcmp(ap.ssid, "Net") == 0);
}

ZTEST(wifi_ap, test_set_ssid_clamps_and_terminates)
{
	struct wifi_ap ap;
	uint8_t src[40];

	memset(src, 'X', sizeof(src));
	wifi_ap_set_ssid(&ap, src, sizeof(src));
	zassert_equal(strlen(ap.ssid), M5WIFI_SSID_MAX_LEN, "clamped to 32");
	zassert_equal(ap.ssid[M5WIFI_SSID_MAX_LEN], '\0', "always terminated");
}

ZTEST(wifi_ap, test_set_ssid_null_src)
{
	struct wifi_ap ap;

	memset(ap.ssid, 'Z', sizeof(ap.ssid));
	wifi_ap_set_ssid(&ap, NULL, 5);
	zassert_equal(ap.ssid[0], '\0', "NULL src yields empty SSID");
}

ZTEST(wifi_ap, test_set_ssid_null_ap)
{
	/* Must not crash. */
	wifi_ap_set_ssid(NULL, (const uint8_t *)"x", 1);
}

ZTEST(wifi_ap, test_set_bssid_full)
{
	struct wifi_ap ap;
	uint8_t mac[M5WIFI_BSSID_LEN] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

	wifi_ap_set_bssid(&ap, mac, sizeof(mac));
	zassert_mem_equal(ap.bssid, mac, M5WIFI_BSSID_LEN, "full MAC copied");
}

ZTEST(wifi_ap, test_set_bssid_short_zerofills)
{
	struct wifi_ap ap;
	uint8_t mac[3] = {1, 2, 3};
	uint8_t zero[M5WIFI_BSSID_LEN] = {0};

	memset(ap.bssid, 0xFF, sizeof(ap.bssid));
	wifi_ap_set_bssid(&ap, mac, sizeof(mac));
	zassert_mem_equal(ap.bssid, zero, M5WIFI_BSSID_LEN, "short MAC zero-filled");
}

ZTEST(wifi_ap, test_set_bssid_null_ap)
{
	/* Must not crash. */
	wifi_ap_set_bssid(NULL, (const uint8_t *)"123456", 6);
}

ZTEST(wifi_fsm, test_reset_from_failed)
{
	struct wifi_fsm f;

	wifi_fsm_init(&f, 2, 100, 800);
	wifi_fsm_connect_requested(&f);
	wifi_fsm_connect_result(&f, false);
	wifi_fsm_connect_requested(&f);
	wifi_fsm_connect_result(&f, false);
	zassert_equal(f.state, M5WIFI_STATE_FAILED);

	wifi_fsm_reset(&f);
	zassert_equal(f.state, M5WIFI_STATE_IDLE);
	zassert_equal(f.attempts, 0);
	zassert_equal(f.backoff_ms, 100, "backoff back to base");
	zassert_equal(f.max_attempts, 2, "policy preserved");
}
