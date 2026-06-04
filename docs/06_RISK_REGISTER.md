# Risk Register

| Risk | Impact | Probability | Mitigation |
|---|---:|---:|---|
| Board DTS does not compile on Zephyr 4.4 | High | Medium | Compare against current upstream boards before editing |
| LCD is ST7789P3, but Zephyr binding is ST7789V-oriented | Medium | Medium | Start with compatible driver, tune init properties empirically |
| ESP32-S3 USB/serial console configuration differs from skeleton | High | Medium | First milestone only targets boot + console using nearby ESP32-S3 boards |
| M5PM1 has no upstream Zephyr driver | Medium | High | Treat as experimental; use M5PM1 docs/library as protocol source |
| ES8311 support is incomplete | Medium | High | First release only needs roadmap/register read; full audio later |
| IR RX conflicts with speaker amplifier | Medium | Medium | Follow vendor warning: disable speaker amp before IR RX tests |
| Overclaiming “first” damages credibility | High | Medium | Use precise prior-art wording and keep evidence directory |
| Large automated edits drift from scope | Medium | Medium | Use `CONTRIBUTING.md`, small backlog stories, and `bash verify.sh` |

## Update 2026-05-30 (after hardware bring-up)

| Risk | Status | Notes / Mitigation |
|---|---|---|
| Board DTS does not compile on Zephyr 4.4 | **Resolved** | Rebuilt to HWMv2 (procpu/appcpu); `west build` succeeds; board discoverable |
| ESP32-S3 USB/serial console config differs | **Resolved** | `zephyr,console=&usb_serial`; boot/console runtime-verified (HW-001) |
| Chip stuck in DOWNLOAD mode on USB-Serial/JTAG | **Resolved** | RTS/hard reset does not re-sample GPIO0 strap on USJ; flash with `esptool --after watchdog-reset`. See `docs/10_HARDWARE_FLASHING_NOTES.md` |
| LCD x/y offsets for StickS3 ST7789P3 unknown | **Resolved** | x=52, y=40 from M5GFX source; verified on hardware (panel shows correct full-screen fills) |
| ST7789P3 vs `sitronix,st7789v` binding | **Resolved** | ST7789P3 works under `sitronix,st7789v` (same family); runtime-verified. Panel kept on CS0 (zephyr#100069) |
| M5PM1 gates LCD/peripheral power rail (no driver) | **Resolved** | L3B is a `regulator-fixed` switched via the M5PM1 MFD gpio child (PYG2); LCD lit (HW-003/010). The interim `m5stack,m5pm1-l3b-regulator` driver was superseded by the MFD path and removed |
| BMI270 INT routed via M5PM1 PMIC, not host GPIO | Open | No host `int-gpios`; polled IMU only until an M5PM1 driver exists |
| Octal PSRAM not enabled in v0.1 | Accepted | App does not need PSRAM and boots without it; enable via `CONFIG_ESP_SPIRAM`/`SPIRAM_MODE_OCT` (GPIO33–37) as roadmap |

## Update 2026-06-01 (v0.6 comprehensive demo: M5PM1 MFD / BLE / audio)

| Risk | Status | Notes / Mitigation |
|---|---|---|
| BLE pulls a binary blob (Espressif controller HAL) | **Mitigated** | `CONFIG_BT`/`CONFIG_APP_BLE` are kept OUT of the default `prj.conf`; BLE lives behind `overlay-ble.conf`. The default build (and CI) is blob-free and green; the BLE build documents `west blobs fetch hal_espressif` |
| M5PM1 MFD init-priority / idle-sleep can leave L3B (LCD) dark | **Mitigated** | The L3B `regulator-fixed` enables via the MFD gpio child, so it must init after GPIO_M5PM1; `CONFIG_REGULATOR_FIXED_INIT_PRIORITY=83` gives I2C(50)<MFD(80)<ADC(81)<GPIO(82)<reg-fixed(83)<display(85). The MFD also clears idle-sleep (reg 0x09) and retries the first I2C transfer (local delta vs #109961). Covered by the MFD ztest + HW-010 |
| Audio amp safety (pops / leaving the speaker live) | **Mitigated** | AW8737 amp (PYG3) is off at rest and driven high only for the duration of a beep; the MFD gpio child uses a masked per-pin RMW so toggling PYG3 preserves the PYG2/L3B bit (LCD stayed lit when the amp toggled, HW-006) |
| BLE stops advertising after a connect/disconnect | **Resolved** | Inline `bt_le_adv_start()` in the disconnect callback returned `-EAGAIN` (connection context not yet freed) → device went silently non-discoverable. Fixed by restarting advertising from a `k_work`; re-scan saw fresh adverts after disconnect (HW-012) |

## Update 2026-06-01 (IR bring-up + M5PM1 telemetry finding)

| Risk | Status | Notes / Mitigation |
|---|---|---|
| M5PM1 cannot measure battery current / SoC on-device | **Accepted (hardware limit)** | The M5PM1 register map exposes voltages only (VBAT/VIN/5VINOUT/VREF + temperature + 2 GPIO analog); there is no current, coulomb-counter or SoC register (verified vs the official `m5stack/M5PM1` lib, the datasheet, PR #109961, and Espressif's StickS3 driver). The on-device current + per-state dataset (issue #4) is not achievable; reframed to an external-USB-meter table (coarse) or a host-side VBAT→SoC estimate. RFC #1 reframed accordingly |
| Zephyr 4.4 has no ESP32 RMT driver for IR | **Mitigated** | IR uses stock drivers instead: LEDC PWM carrier (TX, G46) + MCPWM input capture (RX, G42). A proper ESP32 RMT driver is a separate upstream opportunity, not a blocker here |
| IR NEC envelope jitter under Wi-Fi/BT load | **Open (mitigated by design)** | The carrier is HW-generated by LEDC; only the ~560 µs envelope is software-timed (`k_busy_wait` + per-frame IRQ mask) and NEC tolerates ~±20%. Fallback: drive the envelope from a counter alarm (timer0/1) if marginal. Validate on HW (HW-005) |
| IR RX assumes G42 is a demodulated receiver (envelope, not raw 38 kHz) | **Open** | If MCPWM capture shows the 38 kHz carrier rather than the NEC envelope, the RX decode approach must be revisited. Confirm early on hardware |
| IR receive vs speaker amp | **Mitigated** | The AW8737 amp is off at rest (only high during a beep); the rule for RX is "do not beep while capturing" - no audio-gating change needed |
| IR RX only decodes NEC, not other remote protocols | **Accepted (scope)** | The G42 receiver works (a real remote produced ~10k edges in 40 s) and NEC is decoded (on-device loopback). Other protocols (RC5/RC6/SIRC/...) are not decoded - Zephyr has no IR subsystem to host protocol decoders, so adding them is out of scope; non-NEC remotes still register on the "IR act" counter. A proper ESP32 RMT driver + an IR decoder framework would be a separate, larger upstream effort |

## Update 2026-06-02 (Wi-Fi station: scan + connect + DHCP)

| Risk | Status | Notes / Mitigation |
|---|---|---|
| Wi-Fi + BLE assumed to coexist | **Mitigated** | Not a validated Zephyr config; enforced three ways: `APP_WIFI depends on !APP_BLE`, `overlay-wifi.conf` forces `CONFIG_APP_BLE=n`, and a `BUILD_ASSERT(!(IS_ENABLED(CONFIG_WIFI) && IS_ENABLED(CONFIG_BT)))` in main.c. One radio at a time |
| Cannot connect to a WPA3-only AP | **Accepted (build limit)** | WPA2-PSK only: `CONFIG_ESP32_WIFI_ENABLE_WPA3_SAE` pulls an ECC/PSA path that does not build against this workspace's tf-psa-crypto. WPA2 connect is HW-verified (HW-015); WPA3 is a future enhancement |
| Reconnect driven by two owners (FSM + supplicant) | **Resolved** | The bundled ESP-IDF supplicant owns reconnect; the app `wifi_fsm` is a UI-state mirror that never drives retries (`wifi_fsm_should_retry`/backoff exercised only by native_sim). The glue never drives the failure-count path, so the mirror cannot latch a terminal FAILED while the supplicant keeps retrying |
| DHCP started twice | **Resolved** | `CONFIG_ESP32_WIFI_STA_AUTO_DHCPV4=y` is the single DHCP owner; the glue only reads the lease via `NET_EVENT_IPV4_ADDR_ADD` and never calls `net_dhcpv4_start` |
| Scan list incomplete in a dense RF environment | **Accepted (display limit)** | The scan keeps the first 24 APs (first-arrival cap, not strongest-N); fine because connect is by SSID (never from this list) and the page shows the top few after the RSSI sort |
| Scanning while associated disrupts the link | **Mitigated** | The periodic background scan is suppressed once CONNECTED; only an explicit page-entry scan can run while associated |
| Scan stuck in SCANNING (request accepted, no SCAN_DONE) | **Mitigated** | A scan in SCANNING past a timeout is treated as lost and re-issued, so the device keeps scanning instead of freezing. The related v3.7-branch driver latch (a failed scan that never clears `scan_cb`, so all later scans return `-EINPROGRESS` — issue #110290) does NOT affect us: v4.4's `esp32_wifi_scan` clears `scan_cb` on every failure path (PR #106329) |
| Wi-Fi credentials committed by accident | **Mitigated** | SSID/PSK default empty in committed Kconfig (clean checkout = scan-only); real values live only in the gitignored `app/wifi-creds.local.conf` (`*.local.conf`, `.config`, `*.config`, `twister-out*/` all ignored). Only the SSID is logged, never the PSK; no secret is in any tracked file or git history |
| SPIRAM breaks Wi-Fi (R8 silicon) | **Mitigated** | `CONFIG_ESP_SPIRAM` left off for the Wi-Fi build (system heap); scan + connect verified working without it |

## Update 2026-06-05 (audio capture / microphone, HW-016)

| Risk | Status | Notes / Mitigation |
|---|---|---|
| ES8311 ADC capture brought up blind (reference-derived regs, first silicon test) | **Open (issue #6 blocker)** | HW-016: a `0x44 = 0x68` digital-mux loopback runtime-verified the ASDOUT + I2S0 RX + capture DSP (rms=4089/peak=5800), but the shipping real-ADC route (`0x44 = 0x08`) captures rms=0 on both slots during the beep. The ADC capture route is silent and the fault is NOT localized (analog front end / ADC modulator-power-clock / the 0x08 mux routing itself, which only the 0x68 path is proven to route) and NOT separated from an unconfirmed acoustic stimulus (beep audibility unchecked headless). Next discriminating test: a `0x44 = 0x58` (ADCL real-ADC + DACR DAC) capture. #6 stays OPEN; the analog-ADC bring-up is a focused follow-up. Evidence: `evidence/20260605-hw016-audio-*` |
| Re-running the acoustic loopback back-to-back wedges I2S TX | **Mitigated (harness artifact)** | Calling `audio_loopback()` with no idle gap exhausts the TX mem_slab and trips "loopback prequeue failed". The demo only runs it once per AUDIO-page entry, so this is not a demo bug; the evidence harness spaced runs 2.5 s apart. If an always-on capture path is added later, reclaim the slab (DROP) between runs |
