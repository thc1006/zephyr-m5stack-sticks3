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
- `drivers/regulator/regulator_m5pm1.c` + `dts/bindings/regulator/m5stack,m5pm1-l3b-regulator.yaml`
  implement a single-rail regulator: `enable()` drives the chosen M5PM1 GPIO high
  using the vendor M5GFX register sequence (FUNC0/MODE/DRV/OUT, push-pull), and
  init disables PMIC idle-sleep. The rail is marked `regulator-boot-on` and inits
  (priority 75) between I2C (50) and the display (85).
- Register values were cross-verified against the M5PM1 datasheet, the M5PM1
  library header, and M5GFX source, then adversarially reviewed before flashing.

Upstream note: for contribution this is likely refactored into a full MFD +
GPIO-controller (so `regulator-fixed` can drive the rail), matching AXP192/AW9523B
conventions. See `docs/07_UPSTREAM_PLAN.md`.

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
