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
