# ADR 0005 — Engage upstream ESP32 RMT PR #101448 (HW-test on StickS3) instead of a competing driver

## Status

Accepted (2026-06-05). Addresses issue #9 (ESP32 RMT for proper IR + WS2812).

## Context

Issue #9 wants an ESP32 RMT peripheral driver to replace the StickS3 IR
workaround (LEDC ~38 kHz carrier on G46 + GPIO edge-interrupt RX on G42; both
HW-verified, NEC-only) and to enable WS2812 addressable LEDs.

Deep research (2026-06-05, multi-source, adversarially verified) found:

- Upstream Zephyr has **no merged ESP32 RMT driver**, but the work is already in
  progress: feature request **issue #72546** (assigned to Espressif maintainer
  sylvioalves since 2024) and an OPEN, non-draft driver **PR #101448** "Add
  Espressif RMT driver" by joelguittet (opened 2025-12-22, `CHANGES_REQUESTED`).
  An earlier led-strip-RMT attempt PR #102053 was closed.
- #101448 adds a vendor-specific, multi-purpose RMT primitive as a **misc**
  driver (`drivers/misc/espressif_rmt/`, binding `espressif,esp32-rmt`, Kconfig
  `CONFIG_ESPRESSIF_RMT`), ported from ESP-IDF — not a led_strip or IR subsystem
  integration.
- Reviewers (wmrsouza, fabiobaltieri) explicitly **defer** WS2812-over-RMT to a
  separate dependent follow-up `led_strip` backend PR, which the author said he
  would open. led_strip has five backends (SPI, UART, I2S, GPIO, RP2040-PIO) and
  no RMT one.
- There is **no Zephyr IR/infrared subsystem** — IR (NEC/RC5/SIRC) stays
  app-level on top of a generic RMT TX/RX primitive. This validates our existing
  app-level IR approach.
- `hal_espressif` ships the RMT HAL as **blob-free** Apache-2.0 source
  (`rmt_hal.c`/`rmt_hal.h`/`rmt_types.h` + per-SoC `rmt_ll.h`, incl. esp32s3),
  confirmed present in our workspace. `i2s_esp32` / `pwm_mc_esp32` are the
  HAL-wrap templates.
- StickS3 specifics: the addressable-LED "NeoPixel" output is behind the M5PM1
  PMIC (not a direct ESP32 GPIO), while IR TX=G46 / RX=G42 are direct ESP32 pins.
  So on StickS3 the RMT driver's testable value is **IR**, not WS2812.

## Decision

Do **not** write a competing `esp32-rmt` driver, and do **not** write the
led_strip-RMT WS2812 backend (the #101448 author claimed that follow-up, it
depends on #101448 landing, and StickS3 cannot even HW-test WS2812). Instead,
**engage PR #101448 by hardware-testing it on a real M5Stack StickS3**:

1. Build PR #101448's RMT driver for `m5stack_sticks3/esp32s3/procpu`.
2. Drive NEC IR through the RMT driver on the real pins (G46 TX, G42 RX), reusing
   our existing NEC encode/decode and IR hardware.
3. Capture build + flash + serial evidence under `evidence/`.
4. Post a HW-verified test report on #101448 — a real-board validation it
   currently lacks (most esp32 PRs have no on-hardware coverage).

Keep our existing HW-verified LEDC/GPIO IR in the showcase app until the RMT
driver merges; then migrate IR onto RMT as a follow-up.

## Consequences

Positive:

- Non-duplicative, high upstream value: an on-hardware data point that helps
  #101448 progress toward merge.
- Achievable this iteration (test an existing driver vs author a new one).
- Confirms the RMT HAL is blob-free and usable on the StickS3.

Negative:

- Possible cross-version build friction: #101448 targets recent Zephyr main while
  our workspace tracks v4.4.0; may need a newer checkout or a cherry-pick of the
  RMT files (assessed during execution).
- The WS2812 path cannot be HW-validated on StickS3 (M5PM1-gated NeoPixel), so our
  contribution is scoped to IR + the generic RMT primitive.
