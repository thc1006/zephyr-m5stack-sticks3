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
| M5PM1 gates LCD/peripheral power rail (no driver) | **Resolved** | New `m5stack,m5pm1-l3b-regulator` driver drives PYG2 (L3B) high; adversarially reviewed; LCD lit (HW-003) |
| BMI270 INT routed via M5PM1 PMIC, not host GPIO | Open | No host `int-gpios`; polled IMU only until an M5PM1 driver exists |
| Octal PSRAM not enabled in v0.1 | Accepted | App does not need PSRAM and boots without it; enable via `CONFIG_ESP_SPIRAM`/`SPIRAM_MODE_OCT` (GPIO33–37) as roadmap |
