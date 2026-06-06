# Research Snapshot (Updated) — 2026-05-30

This supersedes/extends `docs/00_RESEARCH_SNAPSHOT.md` with a fresh, source-cited
prior-art and best-practices review performed on 2026-05-30, cross-checked
against a physical StickS3.

## 1. Prior art — is there Zephyr support for StickS3? No.

- Direct inspection of `zephyrproject-rtos/zephyr` `boards/m5stack/` shows:
  `m5stack_atom_lite, m5stack_atoms3, m5stack_atoms3_lite, m5stack_core2,
  m5stack_cores3, m5stack_fire, m5stack_nanoc6, m5stack_stamps3, m5stickc_plus,
  stamp_c3`. **No `sticks3` / `stick_s3` / `m5stick_s3`.**
- No open Zephyr PR and no community Zephyr StickS3 repo found.
- Defensible claim: **first public, reproducible, upstream-oriented Zephyr
  board-port for M5Stack StickS3.** (Not "first Zephyr on ESP32-S3".)
- Closest templates: **`m5stack_atoms3`** (ESP32-S3 + ST7789 LCD + button — best
  match), `m5stack_stamps3` (bare module / flash+PSRAM), `m5stack_cores3`
  (multi-I2C + codec + IMU patterns).

Sources: https://github.com/zephyrproject-rtos/zephyr/tree/main/boards/m5stack ,
https://docs.zephyrproject.org/latest/boards/m5stack/m5stack_atoms3/doc/index.html

## 2. Hardware — official PinMap verified (docs.m5stack.com/en/core/StickS3)

All pins used in this repo's DTS were confirmed against the official PinMap:

| Function | Pins / addr | Status |
|---|---|---|
| SoC | ESP32-S3-PICO-1-N8R8, 8MB flash, 8MB **octal** PSRAM | confirmed (also via esptool chip-id on hardware) |
| LCD ST7789P3 | MOSI G39, SCK G40, RS/DC G45, CS G41, RST G21, BL G38; 135x240 | pins confirmed |
| IMU BMI270 | I2C 0x68, SCL G48, SDA G47 | confirmed; **runtime-verified ready** |
| PMIC M5PM1 | I2C 0x6e, SCL G48, SDA G47 | confirmed |
| Audio ES8311 | I2C 0x18; I2S MCLK G18, DOUT G14, BCLK G17, LRCK G15, DIN G16 | confirmed |
| Mic (analog MEMS) | MSM381A3729H9BPC (U21) into ES8311 MIC1P/MIC1N (analog ADC input) | confirmed (schematic V0.6); capture HW-verified (HW-016d, issue #6) |
| Speaker amp | AW8737, gated by M5PM1 PYG3 (`sound_amp`) | confirmed; HW-006 |
| Buttons | KEY1 G11, KEY2 G12 | confirmed |
| IR | TX G46, RX G42 | confirmed |
| Grove PORT.A | G9, G10 | confirmed |

**Gotchas confirmed by research (affect the port):**
1. **BMI270 INT and several control lines are behind the M5PM1 PMIC**, not on
   host ESP32-S3 GPIO (PinMap "PYG4_IMU_INT" sits on the PMIC). => the BMI270 DT
   node must NOT declare host `int-gpios`; polled IMU only until an M5PM1 driver
   exists. (This repo's DTS correctly omits `int-gpios`.)
2. **No Zephyr M5PM1 driver exists.** First boot runs on the default power rails;
   PMIC management is a roadmap item.
3. **LCD x/y offsets are not published** by M5Stack — must be validated on
   hardware (community practice for this 135x240 panel class is ~x=52, y=40).
4. **ST7789P3 is not a distinct Zephyr binding**; it is expected to work under
   `sitronix,st7789v` (same Sitronix command family) — a bring-up assumption to
   verify on hardware.

Sources: https://docs.m5stack.com/en/core/StickS3 ,
https://github.com/m5stack/M5PM1 ,
https://docs.zephyrproject.org/latest/build/dts/api/bindings/sensor/bosch,bmi270-i2c.html

## 3. Board-port best practices (Zephyr 4.4, HWMv2)

- HWMv2 layout per `board_porting.html`; build target is
  `m5stack_sticks3/esp32s3/procpu`. Serial/console is **PROCPU only**.
- **SoC**: `#include <espressif/esp32s3/esp32s3_pico_n8r8.dtsi>` +
  `select SOC_ESP32S3_PICO_N8R8` (both exist upstream — pure board port, no SoC
  work).
- **Octal PSRAM is a Kconfig concern**, not DT: `CONFIG_ESP_SPIRAM=y`,
  `CONFIG_SPIRAM_MODE_OCT=y`, `CONFIG_SPIRAM_SPEED_80M=y` (consumes GPIO33–37).
  Deferred for v0.1 (the validation app does not need PSRAM and it boots without
  it); roadmap to enable for a complete board.
- **Simple boot vs MCUboot/sysbuild**: a single-core, single-image app uses ESP
  simple boot (image at 0x0, no `Kconfig.sysbuild`). MCUboot + `--sysbuild` +
  `partitions_0x0_amp.dtsi` are only needed for OTA or a real appcpu image.
  v0.1 uses simple boot (matches `m5stack_stamps3`).
- **Display**: use `zephyr,mipi-dbi-spi` + `sitronix,st7789v`; keep the panel on
  **CS0** (open ESP32-S3 second-CS MIPI-DBI bug, zephyr#100069).
- **Upstreaming**: per-core `*.yaml` for Twister, `doc/index.rst` + image, DCO
  `Signed-off-by`, small bisectable commits, and **add self to `MAINTAINERS.yml`**
  for the new board path.

Sources: https://docs.zephyrproject.org/latest/hardware/porting/board_porting.html ,
https://docs.zephyrproject.org/latest/boards/espressif/common/soc-esp32s3-features.html ,
https://developer.espressif.com/blog/2024/12/zephyr-how-to-use-psram/ ,
https://docs.zephyrproject.org/latest/contribute/contributor_expectations.html ,
https://github.com/zephyrproject-rtos/zephyr/issues/100069

## 4. Flashing/monitoring over native USB-Serial/JTAG

Key finding: use **`esptool --after watchdog-reset`** to leave download mode on
ESP32-S3 USB-Serial/JTAG; flash from Windows natively (usbipd not required).
Full detail and citations in `docs/10_HARDWARE_FLASHING_NOTES.md`.

## 4b. Large-scale prior-art re-check (2026-05-30, before going public)

Three independent searches (Zephyr upstream + PR/issue/code search; community/web/
forums; GitHub-wide + recency + adjacent RTOS) cross-agree:

- **Defensible claim**: *first public **Zephyr** board port for the M5Stack StickS3
  (m5stack_sticks3, ESP32-S3-PICO-1-N8R8)*. No StickS3 Zephyr board exists in the
  upstream tree, open/closed PRs, issues, or anywhere on GitHub/web.
- **Do NOT claim**:
  - "first Zephyr on ESP32-S3" (S3 SoC + StampS3/AtomS3/CoreS3 long supported).
  - "first RTOS port" (Arduino/ESP-IDF/UiFlow2/MicroPython/ESPHome/Meshtastic/
    TuyaOpen all predate; StickS3's official platforms are Arduino/UiFlow2/
    ESP-IDF/PlatformIO — Zephyr is not listed).
  - **"first M5PM1 Zephyr driver"** — Zephyr PR #109961 (Benjamin Cabé, draft,
    2026-05-27) adds a full M5PM1 MFD/gpio/adc/regulator suite for the *PaperColor*
    board. See §"Upstream strategy" below.
- **Non-Zephyr near-misses to cite honestly** (none are Zephyr):
  - `espressif/esp-claw` — ESP-IDF, has `boards/m5stack/m5stack_sticks3` + M5PM1 power_manager.
  - `tuya/TuyaOpen` PR #574 — ESP-IDF/FreeRTOS, StickS3 BSP (M5PM1/ES8311/ST7789P3).
  - `TactilityProject/Tactility` — FreeRTOS, has a non-Zephyr `m5stack,sticks3.dts`.
- **Adjacent RTOS**: NuttX has ESP32-S3 SoC support but no StickS3 board; no RIOT/Mongoose.
- **Residual risk**: code search indexes only public default branches; private
  forks, Chinese forums (Bilibili/CSDN) and Zephyr Discord history are not fully
  covered. Re-run `gh search prs "sticks3" --repo zephyrproject-rtos/zephyr` and
  `gh search code "name: m5stack_sticks3"` immediately before publishing.

### Upstream strategy (revised after finding PR #109961)
The canonical upstream M5PM1 driver is being introduced by PR #109961 (MFD + gpio +
adc + regulator, `m5stack,m5pm1*` bindings). We accordingly **vendor the #109961
MFD/GPIO/ADC drivers** and gate L3B via a stock `regulator-fixed`; for upstreaming
the StickS3 board we **depend on / reuse #109961** rather than upstream our own.
(The earlier interim `m5stack,m5pm1-l3b-regulator` driver has since been removed.)
See `docs/07_UPSTREAM_PLAN.md`.

Sources: https://github.com/zephyrproject-rtos/zephyr/pull/109961 ,
https://github.com/zephyrproject-rtos/zephyr/tree/main/boards/m5stack ,
https://github.com/espressif/esp-claw , https://github.com/tuya/TuyaOpen/pull/574 ,
https://github.com/TactilityProject/Tactility , https://docs.m5stack.com/en/core/StickS3

## 5. What changed vs the original snapshot

- Prior-art re-confirmed (still first; date-stamped 2026-05-30).
- Pin table independently verified against the live official PinMap.
- Added the PMIC-routing, PSRAM-Kconfig, ST7789P3, LCD-offset, and
  watchdog-reset findings, each with sources.
