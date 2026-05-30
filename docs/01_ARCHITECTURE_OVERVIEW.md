# Architecture Overview

## Target outcome

A Zephyr out-of-tree application repository that can later be converted into upstream Zephyr patches.

## Layers

```text
Application validation app
  ├─ console/status shell
  ├─ button event test
  ├─ display test page
  ├─ BMI270 polling test
  └─ future: M5PM1, IR, ES8311 audio

Zephyr subsystems
  ├─ GPIO keys
  ├─ SPI / MIPI DBI / display ST7789V
  ├─ I2C / sensor BMI270
  ├─ I2S / audio codec experimental path
  ├─ ESP32 RMT experimental path
  └─ regulator / PMIC experimental path

Board definition
  ├─ board.yml
  ├─ board DTS
  ├─ defconfig
  └─ documentation

ESP32-S3 SoC support
  └─ upstream Zephyr + Espressif support
```

## Bring-up order

1. Board target discoverability.
2. CPU/clock/flash/USB console boot.
3. Buttons.
4. LCD backlight and display.
5. BMI270 polling.
6. Power/EXT_5V behavior.
7. IR TX/RX.
8. Audio capture/playback.

## Evidence model

Each hardware milestone needs:

- Build log.
- Flash log.
- Serial console log.
- Photo/video.
- Exact git commit hash.
