# Validation Matrix

Last hardware run: 2026-05-30 on a physical M5Stack StickS3
(ESP32-S3-PICO-1, MAC 70:04:1d:db:ab:b8), Zephyr 4.4.0, Zephyr SDK 1.0.1.

| Area | Status | Evidence | Notes |
|---|---|---|---|
| Prior-art search | Done (2026-05-30) | `docs/00_RESEARCH_SNAPSHOT.md` | No upstream/community Zephyr StickS3 board found — first public port |
| Repo integrity | Passing | `bash verify.sh` | Always runnable |
| Driver unit tests (M5PM1 MFD path) | **Verified** | `tests/drivers/m5pm1_mfd` (native_sim) | I2C emulator + ztest on native_sim, 4/4 pass: idle-sleep disable (reg 0x09), wake-retry, PYG2/L3B enable sequence, VBAT ADC read. Covers the on-board MFD path; load-bearing seeds (dirty regs + first-transfer fault). Replaces the deleted interim-regulator ztest. Logic-verified, not an independent silicon oracle |
| Driver unit tests (ES8311 codec) | **Verified** | `tests/drivers/audio/es8311` (native_sim) | I2C emulator + ztest on native_sim, 9/9 pass: chip-ID (+ wrong-ID warn), 16 kHz/16-bit configure + write order, volume/mute, unsupported format/property rejection, I2C-error propagation. Logic-verified |
| Zephyr workspace bootstrap | Done | host: Zephyr 4.4.0 + SDK 1.0.1 (WSL2 Ubuntu) | `scripts/bootstrap_zephyr_ubuntu.sh` |
| Board target discovery | **Verified** | `west boards` lists `m5stack_sticks3` | HWMv2 layout, `esp32s3/procpu` + `esp32s3/appcpu` |
| Build validation | **Verified** | `evidence/20260530-0345-build.log` | `west build -b m5stack_sticks3/esp32s3/procpu app`; FLASH 1.64% |
| Flash validation | **Verified** | `evidence/20260530-0432-flash.log` | esptool `write-flash 0x0` `--after watchdog-reset`; hash verified |
| Boot console | **Verified PASS (HW-001)** | `evidence/20260530-0432-serial.log` | USB-Serial/JTAG console; stable uptime, no reset loop |
| Buttons G11/G12 | **Verified PASS (HW-002)** | `evidence/20260530-buttons.log`, `evidence/20260530-b34.log` | gpio-keys input events confirmed for BOTH keys (`code=11` + `code=2`). Physical map (facing device): MIDDLE=KEY1/G11, RIGHT=KEY2/G12, LEFT=power (PMIC, reboots) |
| Regulator disable (HW) | **Verified (B-3)** | `evidence/20260530-b34.log` | `regulator_disable`/`enable` blinks the backlight off/on on hardware (user-confirmed) |
| BMI270 IMU | **Verified PASS (HW-004)** | `evidence/20260530-imu.log` | I2C0 G47/G48, BMI270@0x68; live accel tracks gravity across X/Y/Z on rotation. INT is behind M5PM1 (no host int-gpios) |
| M5PM1 PMIC (LCD rail) | **Verified** | `evidence/20260601-hw010-p3-battery-boot.log` | L3B rail is a `regulator-fixed` switched via the M5PM1 **MFD** gpio child (PYG2) — vendored #109961 MFD/GPIO/ADC + a local idle-sleep/wake-retry fix. Powers the LCD on hardware (HW-010). The interim single-rail `m5stack,m5pm1-l3b-regulator` driver was superseded and removed |
| LCD ST7789P3 | **Verified PASS (HW-003)** | `evidence/20260530-lcd.log` + photos `evidence/PXL_20260529_2351*.jpg` | mipi-dbi-spi + sitronix,st7789v, x/y offset 52/40, inversion ON; panel cycles R/G/B/W (photos show green + red frames), `disp=ready` |
| M5PM1 power (full) | Partial | `evidence/20260601-hw010-p3-battery-boot.log` | Via the MFD: GPIO child (L3B/PYG2 rail + PYG3 audio-amp gate) and ADC child (VBAT, HW-010) done. **The M5PM1 silicon exposes voltages only — no battery/charge current, coulomb counter or SoC register** (verified against the official register map + #109961 + Espressif's driver), so on-device current / per-state current (issue #4) is not obtainable; only charge-enable + power-source/insertion status remain as real "full PMIC" additions |
| IR TX (NEC) | **Verified PASS (HW-005)** | `evidence/20260601-ir-hw-session.md` | TX via the LEDC ~38 kHz carrier (G46); emits NEC frames on hardware (serial `ir_tx` + phone camera), LCD stays lit. NEC encode/decode native_sim ztest 9/9. Gated `CONFIG_APP_IR` (no blob) |
| IR RX (NEC) | **Verified PASS (HW-005)** | `evidence/20260601-ir-rx-edges.log` | GPIO edge interrupt on G42 (MCPWM capture drops edges on the fast NEC burst) + NEC decode. Receiver confirmed: a real remote made ~10k edges in 40 s; NEC decoded via on-device TX→RX loopback. Non-NEC remotes still register on the "IR act" counter; other protocol decoders are out of scope (no Zephyr IR subsystem). Gated `CONFIG_APP_IR` |
| ES8311 audio | **Verified (P5)** | see "ES8311 audio (P5, v0.6)" row below | Codec 0x18 + I2S; gated `CONFIG_APP_AUDIO` (out of scope for the default/v0.1 build) |
| Demo skeleton (P0, v0.6) | **Verified PASS (HW-007)** | `evidence/20260531-hw007-p0-boot.log` + user-confirmed nav | Modular app (main/ui/status/pages) + atomic page index + instant (semaphore-woken) button nav; KEY1 next / KEY2 prev cycling HOME/IMU/DIAG. Button input events themselves logged in HW-002 |
| Text UI (P1, v0.6) | **Verified PASS (HW-008)** | `evidence/20260531-hw008-p1-text-boot.log` + user-confirmed | RGB565 font blitter (vendored cfb_font_1016) renders legible upright text; PAGE_HOME shows title + live uptime; byte order auto-detected RGB_565 |
| Live pages (P2, v0.6) | **Verified PASS (HW-009)** | `evidence/20260531-hw009-p2-pages-boot.log` + user-confirmed | 4 pages (HOME/IMU/POWER/DIAG) + page-name header; live IMU accel xyz with correct signs; flicker-free (clear only on page change); KEY1 next / KEY2 prev |
| Battery / M5PM1 MFD+ADC (P3, v0.6) | **Verified PASS (HW-010)** | `evidence/20260601-hw010-p3-battery-boot.log` + user-confirmed VBAT 4182 mV | M5PM1 restructured to vendored #109961 MFD + ADC + GPIO; L3B rail now `regulator-fixed` on the gpio child; VBAT via Zephyr ADC API (4182 mV, matches issue #4); LCD still lit through the new path; MFD adds idle-sleep + wake-retry (local delta vs #109961) |
| BLE advert + GATT (P4, v0.6) | **Verified PASS (HW-011/012)** | `evidence/20260601-hw011-012-p4-ble.md` (PC bleak) | Connectable adv "StickS3" + decoded manufacturer telemetry (uptime/bat/accel); custom GATT service read + ~1 Hz notify; adv-restart-after-disconnect bug found on HW and fixed (k_work-deferred bt_le_adv_start). Gated `CONFIG_APP_BLE`; default build blob-free |
| ES8311 audio (P5, v0.6) | **Verified PASS (HW-006/013)** | `evidence/20260601-hw006-p5-audio-boot.log` + user-confirmed beep | In-repo ES8311 codec driver (Zephyr audio codec API, native_sim ztest 9/9) + I2S + AW8737 amp via MFD gpio child; 440 Hz beep audible on the AUDIO page; LCD stayed lit when the amp toggled (masked write preserves L3B). Gated `CONFIG_APP_AUDIO`. Also the upstream task #21 driver |
| Upstream PR | Pending | — | After button/LCD evidence; split per `docs/07_UPSTREAM_PLAN.md` |

## Verification levels (per `CONTRIBUTING.md`)

- **scaffolded** — file exists, not built.
- **build-verified** — compiles under Zephyr 4.4 (board discovery + `west build`).
- **flash-verified** — flashes to hardware with verified hash.
- **runtime-verified** — observed running on hardware via serial/photo/video.
- **upstream-reviewed** — accepted/feedback from Zephyr maintainers.

As of 2026-05-30 (runtime-verified on hardware): board discovery, build, flash,
boot/console (HW-001), buttons G11/G12 (HW-002), BMI270 IMU (HW-004), and the
ST7789P3 LCD (HW-003) — the LCD via a new M5PM1 PMIC regulator driver. Remaining
roadmap: full M5PM1 (charger/GPIO/audio rails), IR, and ES8311 audio.
