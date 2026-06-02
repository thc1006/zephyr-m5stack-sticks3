# ADR 0001: Wi-Fi station mode (scan + connect) on M5Stack StickS3

- Status: Accepted
- Date: 2026-06-02
- Deciders: project owner (thc1006)
- Context tags: ESP32-S3, Zephyr 4.4, networking, upstreamable follow-up

## Context

The StickS3 is an ESP32-S3-PICO-1-N8R8. The SoC has a 2.4 GHz Wi-Fi radio, and
Zephyr ships a native driver for it (`drivers/wifi/esp32`, compatible
`espressif,esp32-wifi`). Today our project has BLE working and HW-verified, but
Wi-Fi is unimplemented: the only trace is `&wifi { status = "okay"; }` in the
out-of-tree board DTS, with no driver config, no application, and no
verification. That enabled-but-unverified node also conflicts with our own rule
"keep experimental devices disabled until verified".

We want a real, evidence-backed Wi-Fi station demo (scan, connect, obtain a DHCP
IPv4 lease) that fits our modular gated-app pattern, is unit-tested where
possible, and can later seed an upstream follow-up that enables Wi-Fi on the
in-tree board.

Research summary (Zephyr v4.4.0 source + upstream issues, 2026-06-02):

- Driver: `drivers/wifi/esp32/`, DT node `wifi: wifi { compatible =
  "espressif,esp32-wifi"; status = "disabled"; }` in
  `dts/xtensa/espressif/esp32s3/esp32s3_common.dtsi`.
- Enable: `CONFIG_WIFI`, `CONFIG_WIFI_ESP32`, `CONFIG_NET_L2_WIFI_MGMT`,
  `CONFIG_NETWORKING`, `CONFIG_NET_IPV4`, `CONFIG_NET_DHCPV4`. The driver
  auto-adds a Wi-Fi heap pool (`HEAP_MEM_POOL_ADD_SIZE_ESP_WIFI`, 51200 bytes in
  system-heap mode).
- WPA supplicant: the esp32 native driver uses the ESP-IDF supplicant bundled in
  hal_espressif, NOT Zephyr's hostap-based `CONFIG_WIFI_NM_WPA_SUPPLICANT`
  (enabling the latter conflicts with the bundled one, and the upstream wifi
  shell sample omits it). The station build is WPA2-PSK only: WPA3-SAE pulls an
  ECC/PSA crypto path that does not build against this workspace's tf-psa-crypto.
- Blobs: Wi-Fi RF and 802.11 stack libraries ship as binary blobs in
  `hal_espressif` (`libnet80211.a`, `libpp.a`, `libcore.a`, shared `libphy.a`).
  `west blobs fetch hal_espressif` (already run for BLE) covers Wi-Fi too; no
  extra fetch. Missing blobs show up as linker errors `cannot find -lnet80211`.
- PSRAM pitfall: on ESP32-S3 R8 boards (same silicon as ours), enabling
  `CONFIG_ESP_SPIRAM` has repeatedly broken Wi-Fi (empty scan / OOM): upstream
  issues #74246, #86722, #51364. We do not enable SPIRAM anywhere today, so the
  default system-heap path is the safe one. Keep SPIRAM off for Wi-Fi bring-up.
- Coexistence: Wi-Fi + BLE simultaneous operation is NOT a validated Zephyr
  configuration (the IDF coexistence plumbing exists in the HAL but is not wired
  up or documented at the Zephyr board/network level). Run one radio at a time.
- Maturity: plain station scan/connect is the most-used and working path; AP and
  AP+STA are less stable. Several v4.0-era regressions are fixed in recent
  Zephyr + hal_espressif, so keep both current and re-fetch blobs.

## Decision drivers

1. Honesty ladder: scaffolded -> build-verified -> flash-verified ->
   runtime-verified, with captured evidence.
2. Reuse the existing modular gated-app pattern (overlay + `CONFIG_APP_*` +
   a UI page), as used for BLE, audio, and IR.
3. Maximise what is unit-testable on native_sim (TDD), keeping the RF-coupled
   parts thin and HW-verified.
4. Small, reviewable, upstreamable end state.
5. Avoid the known ESP32-S3 Wi-Fi pitfalls (PSRAM, missing supplicant, heap).

## Considered options

### Mode
- A. Station only (scan + connect + DHCP). Chosen.
- B. Station + SoftAP / AP+STA. Rejected for v1: less mature in Zephyr (#81744,
  #99865), larger, not needed to prove the radio.

### Radio coexistence with BLE
- A. Mutually exclusive gated builds (`overlay-wifi` XOR `overlay-ble`). Chosen.
- B. Wi-Fi + BLE coexistence in one build. Rejected: not validated in Zephyr;
  would be an unverifiable claim.

### Memory / heap
- A. System heap (SPIRAM off) for bring-up. Chosen (evidence: PSRAM breaks
  Wi-Fi on R8). 
- B. Wi-Fi heap/net buffers in PSRAM. Deferred: revisit only after STA is
  verified, pinning `ESP_WIFI_HEAP_SPIRAM` / `ESP32_WIFI_NET_ALLOC_SPIRAM` on
  our exact tree.

### Code structure (for TDD)
- A. Split into a pure-logic module (`app/src/wifi.c` + `wifi.h`,
  native_sim-testable) plus a thin RF glue (`app/src/wifi_glue.c`, the
  `wifi_mgmt`/`net_mgmt` wiring, HW only). Chosen. Mirrors the `nec.c` (pure)
  under `ir.c` (hardware) split that already works in this repo.
- B. One monolithic HW module. Rejected: nothing unit-testable, no TDD.

### Credentials
- SSID/PSK come from Kconfig (`CONFIG_APP_WIFI_SSID` / `CONFIG_APP_WIFI_PSK`) so
  real credentials stay in a local, untracked overlay and never get committed.
  Scan needs no credentials.

## Decision outcome

Implement a gated Wi-Fi station feature:

- `overlay-wifi.conf` turns on `CONFIG_APP_WIFI` plus the Wi-Fi + networking
  Kconfig (station, DHCPv4; the driver's bundled ESP-IDF supplicant, not
  hostap), SPIRAM left off, mutually exclusive with `overlay-ble.conf`.
- Board DTS keeps `&wifi { status = "okay"; }` only meaningfully in the Wi-Fi
  build; the upstream-minimal board stays without it until verified.
- `app/src/wifi.c` / `wifi.h`: pure logic (config validation, security and
  status strings, RSSI-to-bars, scan dedupe/sort, a connection state machine
  with bounded retry and exponential backoff). Unit-tested on native_sim.
- `app/src/wifi_glue.c`: registers `net_mgmt` callbacks, issues
  `NET_REQUEST_WIFI_SCAN` / `NET_REQUEST_WIFI_CONNECT`, feeds events into the
  pure logic, starts DHCPv4, and exposes state to the UI. HW only.
- A `PAGE_WIFI` UI page: shows scan results (SSID + RSSI bars + security),
  connection state, and the DHCP IP once connected.

## TDD plan (red -> green -> refactor)

The pure-logic module is the TDD target; the RF glue is HW-verified, not
unit-tested (the esp32 wifi driver does not build on native_sim).

Test suite `tests/drivers/wifi/` (native_sim, mirrors `tests/drivers/ir_nec/`):

- `wifi_cfg`: SSID length 1..32, PSK 8..63 / SAE 8..128, open ignores PSK,
  unknown security rejected, combined `wifi_cfg_validate` round-trip.
- `wifi_sec`: `wifi_sec_needs_psk`, `wifi_sec_str` for each type (never NULL).
- `wifi_rssi`: `wifi_rssi_bars` boundaries (-55/-65/-75/-85 dBm thresholds).
- `wifi_scan`: dedupe by BSSID keeping strongest RSSI; sort by RSSI descending.
- `wifi_fsm`: init state, connect attempt counting, exponential backoff doubling
  and cap, give-up after max attempts, success resets, disconnect returns to
  idle, `wifi_fsm_should_retry`.

Cycle: write the failing suite (RED), implement `wifi.c` to pass (GREEN), then
refactor for clarity (REFACTOR) keeping the suite green. Every commit runs the
suite on native_sim in CI, same as the NEC and ES8311 tests.

## Development phases

Status (2026-06-02): all phases below (T1 through H4) are complete. The pure logic
is native_sim-tested (`tests/drivers/wifi`, 52 cases) and scan + connect+DHCP are
runtime-verified on hardware (validation matrix HW-014/HW-015).

- WIFI-T1 (RED): add `wifi.h` contract, the native_sim test suite, and a stub
  `wifi.c`; confirm the suite fails. (done)
- WIFI-T2 (GREEN): implement `wifi.c`; suite passes (`bash verify.sh` + the
  native_sim twister run).
- WIFI-T3 (REFACTOR): tidy `wifi.c`, keep green.
- WIFI-H1 (build-verified): `overlay-wifi.conf` + `CONFIG_APP_WIFI` Kconfig +
  app/CMakeLists wiring + DT `&wifi` okay; build for
  `m5stack_sticks3/esp32s3/procpu`.
- WIFI-H2 (flash + runtime: scan): `wifi_glue.c` scan path + `PAGE_WIFI`; flash;
  capture serial scan output (list of nearby APs). Evidence under `evidence/`.
- WIFI-H3 (runtime: connect + DHCP): connect to a known AP via Kconfig creds;
  capture serial CONNECT_RESULT + the DHCP IPv4 address. Photo/serial evidence.
- WIFI-H4 (docs + upstream prep): update SDD/TDD/validation matrix/risk register;
  draft the upstream follow-up that enables `&wifi` on the in-tree board +
  documents Wi-Fi in `board-supported-hw`, with the captured evidence.

## Validation and evidence

- native_sim: `tests/drivers/wifi/` green (build-verified logic). Run with
  `west twister -p native_sim -T tests/drivers/wifi`; note `verify.sh` does not
  run the ztests (it only checks file integrity and shell syntax), same as the
  existing nec/es8311/m5pm1 suites.
- Hardware (HW-0xx ids in the validation matrix): scan list captured on serial;
  connect result + DHCP IP captured on serial; photo/video of the WIFI page.
- Honesty: until WIFI-H2/H3 produce logs, Wi-Fi stays graded "scaffolded /
  build-verified", never "works".

## Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| PSRAM breaks Wi-Fi (R8 silicon, #74246) | Keep `CONFIG_ESP_SPIRAM` off for the Wi-Fi build; system heap only. |
| Cannot connect to secured AP | The esp32 native driver uses its own ESP-IDF supplicant; do NOT enable `CONFIG_WIFI_NM_WPA_SUPPLICANT` (it conflicts with the HAL's bundled one). The build is WPA2-PSK only (`CONFIG_ESP32_WIFI_ENABLE_WPA3_SAE` does not build against this workspace's tf-psa-crypto). Verify the connect at H3. |
| Heap / net-buffer OOM ("Data buffer allocation failed") | Start from the wifi-shell sample buffer sizes; raise `CONFIG_HEAP_MEM_POOL_SIZE` if needed. |
| net_mgmt / workqueue stack overflow on connect (#60819) | Set `CONFIG_NET_MGMT_EVENT_STACK_SIZE` / `CONFIG_NET_TCP_WORKQ_STACK_SIZE` >= 2048. |
| Wi-Fi + BLE assumed to coexist | They do not (not validated); enforce in Kconfig (`APP_WIFI depends on !APP_BLE`), not by convention alone (WIFI-H1). |
| FSM retry duplicates the driver/supplicant reconnect | **Resolved (H3)**: the bundled ESP-IDF supplicant owns reconnect; the app `wifi_fsm` is a UI-state mirror that never drives retries. The glue never drives the failure-count path, so the mirror cannot latch a terminal FAILED while the supplicant keeps retrying. `wifi_fsm_should_retry`/backoff are exercised only by native_sim. |
| DHCP started twice | **Resolved (H3)**: `CONFIG_ESP32_WIFI_STA_AUTO_DHCPV4=y` is the single DHCP owner; the glue only reads the lease via `NET_EVENT_IPV4_ADDR_ADD` and never calls `net_dhcpv4_start`. |
| Missing blobs (linker `-lnet80211`) | `west blobs fetch hal_espressif` (already done for BLE). |
| SMP incompatible with esp32 wifi | procpu target is single-core (AMP procpu/appcpu); confirm `CONFIG_SMP=n`. |

## Upstream path

The esp32 wifi driver is already upstream, so the contribution is board-level:
enable `&wifi` on `m5stack_sticks3`, document Wi-Fi under
`zephyr:board-supported-hw`, and back it with the captured scan/connect
evidence. This lands as a focused follow-up PR after the local demo is
runtime-verified, consistent with the LCD/PMIC/audio/IR roadmap.

## References

- Zephyr Wi-Fi management API: docs.zephyrproject.org/latest/services/connectivity/networking/api/wifi.html
- Espressif binary blobs / system requirements: docs.zephyrproject.org/latest/boards/espressif/common/system-requirements.html
- Wi-Fi shell sample: samples/net/wifi/shell
- Upstream pitfalls: zephyr issues #74246, #86722, #51364 (PSRAM), #60819 (stacks), #62861 (heap/net-buf), #81744 (apsta)
