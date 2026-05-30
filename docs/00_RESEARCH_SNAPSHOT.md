# Research Snapshot — 2026-05-30

## Summary

This project is positioned as an **M5Stack StickS3-specific Zephyr enablement effort**, not as a generic ESP32-S3 novelty claim.

## Public facts to ground the project

- M5Stack says StickS3 is powered by **ESP32-S3-PICO-1-N8R8**, supports 2.4 GHz Wi-Fi, and includes **8MB Flash + 8MB PSRAM**.
- M5Stack lists built-in **1.14 inch LCD**, **BMI270 6-axis IMU**, **ES8311 audio codec**, MEMS microphone, speaker amplifier, IR TX/RX, programmable buttons, HY2.0-4P and Hat2-Bus expansion.
- The official M5Stack platform list is Arduino, UiFlow2, ESP-IDF, and PlatformIO. Zephyr is not listed as an official platform in the product docs.
- Zephyr already supports ESP32-S3-class hardware and multiple M5Stack boards, so the claim must be board-specific.
- Zephyr 4.4.0 is the latest stable release in the 2026-05-30 snapshot; Zephyr docs list 4.4.0 as released 2026-04-14 and latest stable, with 4.5 targeted for October 2026.

## Hardware pin table copied into implementation plan

| Function | Component | ESP32-S3 pins / address |
|---|---|---|
| LCD | ST7789P3 | G39 MOSI, G40 SCK, G45 RS/DC, G41 CS, G21 RST, G38 BL |
| IMU | BMI270 | I2C 0x68, G48 SCL, G47 SDA |
| PMIC / power | M5PM1 | I2C 0x6e, G48 SCL, G47 SDA; PYG0 charge status, PYG1 IRQ, PYG2 L3B power, PYG3 speaker pulse, PYG4 IMU INT |
| Audio | ES8311 | I2C 0x18, G18 MCLK, G14 DOUT, G17 BCLK, G15 LRCK, G16 DIN, G48 SCL, G47 SDA |
| Buttons | KEY1/KEY2 | G11, G12 |
| IR | IR_TX / IR_RX | G46, G42 |
| HY2.0-4P | PORT.CUSTOM | G9, G10 |
| Hat2-Bus | expansion | G5, G4/EXT_5V, Boot, G6, G1, G7, G8, G43, BAT/G44, G2/3V3_L2, G3/5V_IN |

## Defensible public wording

Use:

> I could not find an official or complete public Zephyr board target for M5Stack StickS3 / M5StickS3 K150 in my 2026-05-30 prior-art search. This project provides an upstream-oriented board-port scaffold and validation plan.

Do not use:

> World's first Zephyr on ESP32-S3.

## Main technical bets

1. Treat `m5stack_sticks3` as an out-of-tree board first.
2. Reuse Zephyr ESP32-S3 SoC support instead of writing SoC code.
3. Reuse existing display/sensor subsystems first: ST7789V binding/driver and BMI270 binding/driver.
4. Treat M5PM1 and ES8311 as higher-risk because they may require new or adapted drivers.
5. Split upstream PRs to avoid a giant review burden.

## Sources to re-check before public release

See `docs/09_SOURCES.md` for source URLs and exact claims.
