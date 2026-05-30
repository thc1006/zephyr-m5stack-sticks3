# Validation Matrix

Last hardware run: 2026-05-30 on a physical M5Stack StickS3
(ESP32-S3-PICO-1, MAC 70:04:1d:db:ab:b8), Zephyr 4.4.0, Zephyr SDK 1.0.1.

| Area | Status | Evidence | Notes |
|---|---|---|---|
| Prior-art search | Done (2026-05-30) | `docs/00_RESEARCH_SNAPSHOT.md` | No upstream/community Zephyr StickS3 board found — first public port |
| Repo integrity | Passing | `bash verify.sh` | Always runnable |
| Driver unit tests (M5PM1) | **Verified** | `evidence/20260530-ztest-m5pm1.log` | I2C emulator + ztest on native_sim, 6/6 pass; adversarially hardened (dirty seeds, full re-assert, write-order, neighbour-preserve, I2C-error injection). Logic-verified, not an independent silicon oracle |
| Zephyr workspace bootstrap | Done | host: Zephyr 4.4.0 + SDK 1.0.1 (WSL2 Ubuntu) | `scripts/bootstrap_zephyr_ubuntu.sh` |
| Board target discovery | **Verified** | `west boards` lists `m5stack_sticks3` | HWMv2 layout, `esp32s3/procpu` + `esp32s3/appcpu` |
| Build validation | **Verified** | `evidence/20260530-0345-build.log` | `west build -b m5stack_sticks3/esp32s3/procpu app`; FLASH 1.64% |
| Flash validation | **Verified** | `evidence/20260530-0432-flash.log` | esptool `write-flash 0x0` `--after watchdog-reset`; hash verified |
| Boot console | **Verified PASS (HW-001)** | `evidence/20260530-0432-serial.log` | USB-Serial/JTAG console; stable uptime, no reset loop |
| Buttons G11/G12 | **Verified PASS (HW-002)** | `evidence/20260530-buttons.log`, `evidence/20260530-b34.log` | gpio-keys input events confirmed for BOTH keys (`code=11` + `code=2`). Physical map (facing device): MIDDLE=KEY1/G11, RIGHT=KEY2/G12, LEFT=power (PMIC, reboots) |
| Regulator disable (HW) | **Verified (B-3)** | `evidence/20260530-b34.log` | `regulator_disable`/`enable` blinks the backlight off/on on hardware (user-confirmed) |
| BMI270 IMU | **Verified PASS (HW-004)** | `evidence/20260530-imu.log` | I2C0 G47/G48, BMI270@0x68; live accel tracks gravity across X/Y/Z on rotation. INT is behind M5PM1 (no host int-gpios) |
| M5PM1 PMIC (LCD rail) | **Verified** | `evidence/20260530-lcd.log` | New `m5stack,m5pm1-l3b-regulator` driver drives PYG2 (L3B) high; adversarially reviewed before flashing |
| LCD ST7789P3 | **Verified PASS (HW-003)** | `evidence/20260530-lcd.log` + photos `evidence/PXL_20260529_2351*.jpg` | mipi-dbi-spi + sitronix,st7789v, x/y offset 52/40, inversion ON; panel cycles R/G/B/W (photos show green + red frames), `disp=ready` |
| M5PM1 power (full) | Partial | `evidence/20260530-lcd.log` | LCD rail (PYG2) driver done; charger/fuel-gauge/GPIO/audio rails still roadmap |
| IR TX/RX | Roadmap | — | RMT strategy to verify (G46/G42) |
| ES8311 audio | Roadmap | — | Codec 0x18 + I2S; out of scope for v0.1 |
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
