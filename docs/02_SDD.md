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
- I2S (ES8311 audio) is enabled as of v0.6. IR uses the IR TX LED (G46) and the
  IR receiver (G42); since Zephyr 4.4 has no ESP32 RMT driver, IR is brought up
  via the LEDC PWM carrier (TX) and MCPWM input capture (RX), not RMT (v0.7).

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

M5PM1 telemetry limit (verified 2026-06-01 against the official register map, the
datasheet, PR #109961's scope, and Espressif's StickS3 driver — four independent
sources): the M5PM1 ADC exposes **voltages only** (VREF/VBAT/VIN/5VINOUT + an
internal temperature + two GPIO analog inputs). There is **no battery-current,
charge-current, coulomb-counter or state-of-charge register** anywhere in the
map. So battery *current* and a per-power-state current dataset (issue #4, folded
into the full-PMIC RFC #1) are not obtainable on-device; they can only come from
an external USB meter (coarse, whole-board) or a host-side VBAT→SOC estimate.
What the chip does expose for power control is a charge-enable bit (PWR_CFG 0x06
b0), a battery low-voltage cutoff (BATT_LVP 0x08), a power-source readback
(PWR_SRC 0x04: 5VIN/5VINOUT/BAT) and battery insert/remove events (IRQ 0x41).
Any "full PMIC" work is therefore charge-enable + power-source/insertion status,
not a fuel gauge.

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
  needed. `configure()` handles the playback (DAC), capture (ADC) and combined
  `PLAYBACK_CAPTURE` routes. This driver is also the standalone upstream
  candidate (task #21).
- Playback data path: SoC I2S0 as master (16 kHz / 16-bit, standard I2S) →
  ES8311 DAC → AW8737 speaker amp. The app plays a short 440 Hz beep on entering
  the AUDIO page.
- Capture data path (issue #6): on-board analog MEMS mic (MSM381A3729H9BPC) →
  ES8311 MIC1 ADC → SoC I2S0 RX (DIN G16). The driver's ADC block programs a
  single-ended analog MIC1 (0x14), ADC power (0x0E), ADC serial-out format (0x0A),
  ~0 dB ADC volume (0x17) and an ADC high-pass filter (0x1B/0x1C, cancels the
  digital DC offset), with 0x44 = 0x08 keeping ASDOUT as plain ADC data
  (no digital DAC feedback; ESP-ADF's capture default 0x58 mixes a digital DAC
  copy into the captured stream, which we avoid). The analog PGA is at 30 dB max,
  tunable in the follow-up if it clips. Capture is always-on once configured (no
  per-stream codec start), matching the in-tree wm8904/da7212 pattern. The ADC
  register values are reference-derived (ESP-ADF). HW-016 status (2026-06-05):
  the ASDOUT serial output, the I2S0 RX path and the capture DSP are
  runtime-verified via a `0x44 = 0x68` digital-mux loopback (bit-stable
  rms=4089/peak=5800), but the real-ADC route (`0x44 = 0x08`) captures rms=0
  during the beep, so the on-board analog mic is NOT yet validated. The fault is
  not localized (analog front end, ADC modulator/power/clock, or the 0x08 mux
  routing) and is not yet separated from an unconfirmed acoustic stimulus, so
  issue #6 stays open. See `evidence/20260605-hw016-audio-capture.md`.
- Amp enable: the AW8737 is gated by M5PM1 PMIC GPIO3 (PYG3, `sound_amp` /
  `amp-gpios`), driven through the **MFD gpio child** with a per-pin
  read-modify-write so toggling PYG3 preserves the neighbouring PYG2/L3B bit
  (the LCD rail). The amp is driven high ONLY for the duration of a beep
  (anti-pop, speaker muted at rest). The mic and speaker share the L3B rail
  (PYG2), so capture needs L3B powered (already up for the LCD).
- Test: `tests/drivers/audio/es8311` (native_sim ztest, 11/11) covers chip-ID
  read, the playback and capture configure sequences + write ordering,
  volume/mute, unsupported-route/format rejection, and I2C-error propagation.

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

### 4.11 IR transmit + receive (gated `CONFIG_APP_IR`)

Optional consumer-IR (NEC) over the on-board IR TX LED (G46) and IR receiver
(G42), kept off the default build (`app/src/ir.c` compiled only with
`overlay-ir.conf`; all code under `#ifdef CONFIG_APP_IR`). Uses no binary blobs.

Zephyr 4.4 has no ESP32 RMT driver and no consumer-IR subsystem, so IR is built
on stock PWM drivers:

- TX: the ESP32-S3 **LEDC** PWM (`espressif,esp32-ledc`, `&ledc0` channel on
  G46) generates the ~38 kHz carrier (APB 80 MHz / 11-bit ≈ 37.9 kHz, ~33%
  duty). The NEC envelope (9 ms lead, 4.5 ms space, 560 µs base bit unit) is
  produced by gating the carrier duty 0↔N from the CPU with `k_busy_wait()`
  (NEC tolerates ~±20%; IRQs are briefly masked per frame to bound jitter).
- RX: a plain **GPIO edge interrupt** on G42 (the ESP32 MCPWM capture path was
  tried first but drops edges during the fast 67 ms NEC burst). Each edge is
  timestamped with the cycle counter; the level that just ended becomes a mark
  (low) or space (high), and an app-side state machine feeds (mark, space) pairs
  to the NEC decoder. RX is suspended during a TX burst (the device must not
  receive its own carrier, and the RX ISR would otherwise jitter the TX timing).
- Pure-logic split for testability: `app/src/nec.c` holds `nec_encode()` and a
  tolerance-based `nec_decode()`, unit-tested on native_sim
  (`tests/drivers/ir_nec`) with synthetic ideal/jittered/invalid vectors — the
  carrier gating and the capture callback are thin shells around these.
- The AW8737 speaker amp must be OFF during IR receive (vendor warning); it is
  already off at rest (only high during a beep), so the only rule is "do not beep
  while capturing" — no change to the audio gating.
- Demo: entering the PAGE_IR page transmits one test NEC frame (phone-camera
  visible); the page shows the TX/RX counts, the last decoded NEC `addr:cmd`, and
  an "IR act" edge counter that ticks up for ANY remote (any protocol), so a
  non-NEC remote still visibly registers.
- Status (2026-06-01): TX and RX both runtime-verified on hardware. TX emits NEC
  (loopback + phone camera). The G42 receiver works: a real remote produced ~10k
  edges in 40 s (so it receives external IR of any protocol), and the NEC decoder
  recovers addr/cmd from an on-device TX->RX loopback. Decoding non-NEC protocols
  (RC5/RC6/SIRC/...) is out of scope - Zephyr has no IR subsystem to host protocol
  decoders; NEC is the supported reference protocol and other remotes still
  register on the "IR act" counter. See TDD HW-005.

### 4.12 Wi-Fi station: scan + connect (gated `CONFIG_APP_WIFI`)

Optional ESP32-S3 native Wi-Fi in station mode, kept off the default build
(`app/src/wifi.c` + `wifi_glue.c` compiled only with `overlay-wifi.conf`; all
code under `#ifdef CONFIG_APP_WIFI`). Mutually exclusive with BLE — Wi-Fi + BLE
coexistence is not a validated Zephyr configuration on the ESP32-S3, enforced by
`APP_WIFI depends on !APP_BLE`, `overlay-wifi.conf` forcing `CONFIG_APP_BLE=n`,
and a `BUILD_ASSERT(!(IS_ENABLED(CONFIG_WIFI) && IS_ENABLED(CONFIG_BT)))` in
main.c. Needs the `hal_espressif` blob (already fetched for BLE).

- Driver / supplicant: the in-tree `drivers/wifi/esp32` driver (DT
  `espressif,esp32-wifi`, `&wifi` enabled by `overlay-wifi.overlay`) with its
  bundled ESP-IDF supplicant — NOT Zephyr's hostap `CONFIG_WIFI_NM_WPA_SUPPLICANT`
  (the two conflict). WPA2-PSK only; WPA3-SAE does not build against this
  workspace's tf-psa-crypto. SPIRAM left off (it breaks Wi-Fi on R8).
- Pure-logic split for testability: `app/src/wifi.c` holds config validation,
  security/RSSI/bars labels, scan dedupe (by BSSID, keep strongest) + RSSI sort,
  and a connection state machine, unit-tested on native_sim
  (`tests/drivers/wifi`, 52 cases). `app/src/wifi_glue.c` is the thin
  `net_mgmt`/`wifi_mgmt` shell (scan + connect requests, event callbacks, IPv4
  capture), HW only — mirrors the nec.c (pure) under ir.c (HW) split.
- Scan: `NET_REQUEST_WIFI_SCAN` → `SCAN_RESULT`/`SCAN_DONE` feed the pure
  dedupe+sort; up to 24 APs (first-arrival cap, not strongest-N — fine because
  connect is by SSID and the page shows the top few). The result dump is paced
  (`k_msleep`) so the ESP32-S3 USB-Serial/JTAG TX ring buffer is not overwritten
  mid-burst. A periodic background scan (~15 s) keeps the list fresh and self-
  heals a scan stuck in SCANNING; it is suppressed once CONNECTED (scanning while
  associated drops frames).
- Connect + DHCP: `NET_REQUEST_WIFI_CONNECT` with the validated SSID/PSK →
  `CONNECT_RESULT`/`DISCONNECT_RESULT` drive the UI state; `NET_EVENT_IPV4_ADDR_ADD`
  captures the DHCP lease. Reconnect and DHCP have exactly one owner each: the
  ESP-IDF supplicant owns reconnect (the app FSM is a UI-state mirror that never
  drives retries), and `CONFIG_ESP32_WIFI_STA_AUTO_DHCPV4` owns DHCP (the glue
  never calls `net_dhcpv4_start`).
- Credentials: SSID/PSK come from `CONFIG_APP_WIFI_SSID` / `CONFIG_APP_WIFI_PSK`
  (default empty → scan-only). Real credentials live in a LOCAL, untracked overlay
  (`app/wifi-creds.local.conf`, gitignored); they are baked into the firmware for
  the demo but never committed, and only the SSID is ever logged.
- Demo: a PAGE_WIFI page shows the scan list (SSID + signal bars + security) and,
  once connected, the connection state and the DHCP IPv4 address.
- Status (2026-06-02): scan and connect+DHCP both runtime-verified on hardware
  (24 APs deduped/sorted; associated to a WPA2-PSK AP and obtained a DHCP lease).
  See TDD HW-014/HW-015.

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

### v0.7 — IR (NEC) TX + RX

- IR transmit via the LEDC carrier (G46) + receive via MCPWM capture (G42), gated
  `CONFIG_APP_IR`; NEC encode/decode unit-tested; TX→RX loopback self-test on HW.

### v0.8 — Wi-Fi station (scan + connect + DHCP)

- ESP32-S3 native Wi-Fi station, gated `CONFIG_APP_WIFI` (mutually exclusive with
  BLE); pure scan/connect/config logic unit-tested on native_sim; scan list +
  WPA2-PSK connect + DHCP lease runtime-verified on HW (HW-014/HW-015).
  Credentials kept local and untracked. Seeds an upstream follow-up that enables
  `&wifi` on the in-tree board.
