# Software Design Document — M5StickS3 Zephyr Enablement

## 1. Purpose

Provide a reproducible Zephyr board-port and validation project for M5Stack StickS3 / M5StickS3 K150.

## 2. Design constraints

- Must use upstream Zephyr mechanisms: board directory, devicetree, Kconfig, samples/tests.
- Must avoid custom SoC code unless absolutely necessary.
- Must preserve a path to upstream contribution.
- Must distinguish scaffolded support from physically verified support.
- Must keep contributor instructions concise and tool-agnostic (`CONTRIBUTING.md`).

## 3. Board naming decision

Initial local board target: `m5stack_sticks3`.

Rationale:

- Consistent with existing Zephyr M5Stack names such as `m5stickc_plus`, `m5stack_cores3`, `m5stack_atoms3`, and `m5stack_stamps3`.
- Avoids ambiguous `m5stick_s3` spelling until upstream maintainers give naming feedback.

## 4. Main design components

### 4.1 Board definition

Path: `boards/m5stack/m5stack_sticks3/`

The board follows the Zephyr Hardware Model v2 (HWMv2) layout used by the
nearby upstream ESP32-S3 M5Stack boards (`m5stack_atoms3`, `m5stack_stamps3`,
`m5stack_cores3`). ESP32-S3 is dual-core, so the board exposes `procpu` and
`appcpu` qualifiers (`m5stack_sticks3/esp32s3/procpu`).

Required files:

- `board.yml`
- `board.cmake`
- `Kconfig`
- `Kconfig.defconfig`
- `Kconfig.m5stack_sticks3` (selects `SOC_ESP32S3_PICO_N8R8`)
- `m5stack_sticks3-pinctrl.dtsi`
- `m5stack_sticks3_procpu.dts` / `_procpu.yaml` / `_procpu_defconfig`
- `m5stack_sticks3_appcpu.dts` / `_appcpu.yaml` / `_appcpu_defconfig`
- `doc/index.rst`

The SoC base is `espressif/esp32s3/esp32s3_pico_n8r8.dtsi` (ESP32-S3-PICO-1-N8R8,
8 MB flash + 8 MB PSRAM), confirmed against the local Zephyr 4.4 tree.

### 4.2 Devicetree responsibilities

The DTS should describe:

- chosen console and shell on `&usb_serial` (ESP32-S3 native USB Serial/JTAG).
- GPIO keys for G11 (KEY1) and G12 (KEY2).
- LCD on `spi2` via a `zephyr,mipi-dbi-spi` controller with an ST7789P3 panel
  using the `sitronix,st7789v` driver. The current Zephyr ST7789V binding
  requires a MIPI-DBI parent and `mipi-mode`/`ram-param`/`rgb-param`, so the
  obsolete direct-SPI form from the v0.1 scaffold is not used.
- I2C0 bus on G47 (SDA) / G48 (SCL) with BMI270 at 0x68. M5PM1 (0x6e) and
  ES8311 (0x18) remain roadmap items: M5PM1 has no upstream driver and ES8311
  audio is out of scope for v0.1.
- IR (G46/G42) and I2S (ES8311) pins are documented but not enabled until the
  RMT/audio strategy is verified.

Peripherals are enabled incrementally per milestone: boot/console + buttons +
BMI270 first; LCD and experimental peripherals are added in their own steps.

### 4.3 Application validation

Path: `app/`

The initial app must:

- print boot banner and build metadata.
- check `gpio-keys` readiness if available.
- check BMI270 readiness if the devicetree node is enabled.
- show a simple LCD page if display is enabled.
- fail gracefully if optional devices are not ready.

### 4.4 Testing design

- Host tests validate repository structure, file naming, and pin table consistency.
- Twister build-only tests validate compileability once Zephyr is available.
- Hardware validation uses a manual evidence checklist.

### 4.5 M5PM1 PMIC driver (out-of-tree module)

The LCD/MIC/SPK power domain ("L3B") is gated by the M5PM1 PMIC (I2C 0x6e) via
its GPIO2 (PYG2), not by an ESP32 GPIO. Zephyr has no M5PM1 driver, so the repo
ships one as an out-of-tree module:

- `zephyr/module.yml` + top `CMakeLists.txt`/`Kconfig` register the repo as a
  Zephyr module (`dts_root: .`), pulled in via `ZEPHYR_EXTRA_MODULES`.
- As of P3 the board models the M5PM1 as the upstream-style MFD tree vendored
  from Zephyr PR #109961: an MFD parent (`m5stack,m5pm1`, `pmic@6e`) with a GPIO
  child (`m5stack,m5pm1-gpio`, `ngpios = <5>`) and an ADC child
  (`m5stack,m5pm1-adc`, `#io-channel-cells = <1>`, channel 1 = VBAT). Drivers:
  `drivers/{mfd,adc,gpio}/{mfd,adc,gpio}_m5pm1.c` + matching
  `dts/bindings/{mfd,adc,gpio}/m5stack,m5pm1*.yaml`.
- The L3B rail is now a top-level `regulator-fixed` (`lcd_power`) whose
  `enable-gpios = <&m5pm1_gpio 2 GPIO_ACTIVE_HIGH>`, `regulator-boot-on`,
  `startup-delay-us = <100000>`. This supersedes the interim single-rail
  `m5stack,m5pm1-l3b-regulator` driver + binding, which were removed once the
  MFD path was hardware-verified (HW-010); the board no longer needs an
  M5PM1-specific regulator driver.

Init-priority contract (critical, HW-003 power-before-init): because the L3B
`regulator-fixed` enables through the MFD gpio child, it must init *after* that
gpio child and *before* the display. The `regulator-fixed` default (75) runs
before the gpio child (82), so the enable-gpio write would fail and the LCD
would stay dark. `app/prj.conf` therefore sets
`CONFIG_REGULATOR_FIXED_INIT_PRIORITY=83`, giving the POST_KERNEL order
I2C(50) < MFD(80) < ADC_M5PM1(81) < GPIO_M5PM1(82) < regulator-fixed(83) <
display(85). Verified against the linker `SORT` on the priority-encoded init
sections and the generated `.config`.

Upstream note: the MFD/GPIO/ADC drivers and bindings track Zephyr PR #109961 and
are removed once it merges. See `docs/07_UPSTREAM_PLAN.md`.

### 4.6 Comprehensive demo application architecture (v0.6)

The v0.6 release evolves `app/` from the single-file validation loop into a
modular, multi-page demo that showcases the whole board: a live LCD dashboard
(uptime, battery, IMU), button-driven page navigation, BLE telemetry, and audio.
It is built in hardware-verified phases (P0-P6).

Module layout under `app/src/`:

- `main.c` — thin: init devices, then a single loop `status_sample -> ui_render
  (current page) -> [ble_update] -> sleep`. No heavy work in callbacks.
- `pages.h` — `enum app_page` + an `atomic_t` current-page index; buttons advance
  it via the input callback, which only updates the atomic and never draws.
- `status.{c,h}` — `struct app_status` (uptime, battery mV, IMU accel, readiness)
  + `status_sample()` gathering all telemetry once per loop.
- `ui.{c,h}` (+ later `gfx.{c,h}` / `font.{c,h}`) — page rendering and the RGB565
  text blitter.
- `ble.{c,h}` (gated `CONFIG_APP_BLE`) and `audio.{c,h}` (gated `CONFIG_APP_AUDIO`)
  — optional features kept off the default/CI build so it stays blob-free.

The comprehensive demo lives in-repo as a showcase; upstream contributions stay
small and separate (board port, ES8311 codec, M5PM1 MFD/ADC). Feature gating keeps
the default build blob-free and green; BLE/audio builds require their overlay
config and `west blobs fetch hal_espressif`.

### 4.7 Text rendering (RGB565 font blitter)

The demo renders text without a heavy GUI library. CFB (Zephyr's character
framebuffer) is monochrome-only and cannot drive the RGB565 ST7789, so the app
ships a small blitter (`app/src/gfx.c`) that expands a vendored CFB monochrome
glyph table (`app/src/font.c`, `cfb_font_1016` 10x16, Apache-2.0, attributed)
into RGB565 and pushes it via `display_write`. `gfx_init()` reads
`display_get_capabilities()` to pick RGB_565 (high-byte-first) vs RGB_565X byte
order. Buffers are a one-row fill buffer plus a one-glyph buffer (no full
framebuffer), leaving SRAM headroom for the BT stack. LVGL remains a heavier
fallback only if richer UI is ever needed.

### 4.8 Button input and page navigation

Buttons advance the active page through the Zephyr `input` subsystem, not by
polling in the render loop. `pages.h` holds an `atomic_t` current-page index;
the `gpio-keys` input callback only updates that atomic (KEY1 = next, KEY2 =
prev, wrapping) and posts a semaphore that wakes `main.c`, so a press is
reflected on the next frame without busy-waiting. The callback never draws — all
rendering stays on the main loop — which keeps ISR/callback context minimal and
avoids re-entrancy against `display_write`.

### 4.9 ES8311 audio (gated `CONFIG_APP_AUDIO`)

Optional audio output via the on-board ES8311 codec, kept entirely off the
default build (`app/src/audio.c` is compiled only with `overlay-audio.conf`,
and every line is also under `#ifdef CONFIG_APP_AUDIO`).

- Codec driver: an in-repo ES8311 driver (`drivers/audio/es8311.c`) implemented
  against the Zephyr **audio codec API** (`zephyr/audio/codec.h`):
  `audio_codec_configure()` / `start_output()` / `set_property()`. I2C control
  is at 0x18; the codec runs MCLK-derived-from-BCLK so no separate MCLK pin is
  needed. This driver is also the standalone upstream candidate (task #21).
- Data path: SoC I2S0 as master (16 kHz / 16-bit, standard I2S) → ES8311 →
  AW8737 speaker amp. The app plays a short 440 Hz beep on entering the AUDIO
  page.
- Amp enable: the AW8737 is gated by M5PM1 PMIC GPIO3 (PYG3, `sound_amp` /
  `amp-gpios`), driven through the **MFD gpio child** with a per-pin
  read-modify-write so toggling PYG3 preserves the neighbouring PYG2/L3B bit
  (the LCD rail). The amp is driven high ONLY for the duration of a beep
  (anti-pop, speaker muted at rest).
- Test: `tests/drivers/audio/es8311` (native_sim ztest, 9/9) covers chip-ID
  read, configure sequence + write ordering, volume/mute, unsupported-format
  rejection, and I2C-error propagation.

### 4.10 BLE telemetry (gated `CONFIG_APP_BLE`)

Optional Bluetooth LE telemetry (`app/src/ble.c`, compiled only with
`overlay-ble.conf`; all code also under `#ifdef CONFIG_APP_BLE`). The default
build keeps `CONFIG_BT` OUT so it stays **blob-free and CI-green**; the BLE
build pulls the Espressif controller HAL blob and requires
`west blobs fetch hal_espressif`.

- Advertising: connectable + scannable legacy advertising carrying flags, the
  device name ("StickS3"), and manufacturer-specific data (company 0xFFFF) with
  a packed little-endian payload — uptime (s), battery (mV), accel X/Y/Z
  (milli-g) — so a scanner sees live telemetry without connecting.
- GATT: one custom 128-bit primary service with a telemetry characteristic
  (READ + NOTIFY) plus its CCC, pushing the same payload at ~1 Hz to subscribers.
- Advertising restart: after a client disconnects, advertising is restarted from
  the system workqueue (`k_work`), NOT inline in the disconnect callback —
  calling `bt_le_adv_start()` there returns `-EAGAIN` before the connection
  context is freed, which silently left the device non-discoverable. Deferring
  to `k_work` fixes it (found on hardware, HW-012).

## 5. Non-goals for v0.1

- Full ES8311 audio driver upstreaming.
- Full M5PM1 PMIC feature set (charger, fuel-gauge, GPIO expander, audio rails);
  v0.1 ships only the minimal LCD/L3B rail regulator.
- Deep sleep current optimization.
- Production-grade OTA.

## 6. Release phases

### v0.1 — public scaffold

- Project skeleton.
- Board-port starting point.
- Build scripts.
- SDD/TDD/ADR/backlog.

### v0.2 — build-verified board target

- `west build` succeeds for hello world or validation app.
- Known DTS/Kconfig limitations documented.

### v0.3 — hardware basic validation

- boot, console, buttons.

### v0.4 — display + IMU validation

- ST7789 display demo.
- BMI270 polling demo.

### v0.5 — upstream PR candidate

- cleaned board docs.
- small patchset against Zephyr upstream.

### v0.6 — comprehensive demo

- Modular multi-page demo app (P0).
- RGB565 text UI (P1) + live telemetry pages with button navigation (P2).
- Battery via M5PM1 MFD + ADC (P3, vendors upstream #109961).
- BLE manufacturer-data advertising + connectable GATT telemetry (P4, gated).
- ES8311 codec driver + I2S audio (P5, gated; also upstream task #21).
- Integration, polish, evidence (P6).
