# Hardware Flashing & Monitoring Notes (verified 2026-05-30)

These notes record the **verified** procedure for flashing and observing the
M5Stack StickS3 on Zephyr, plus the one non-obvious gotcha that cost real time.
They were confirmed both empirically (on a physical StickS3) and against
authoritative Espressif/Zephyr documentation.

## Topology

- The StickS3 connects over its **native USB-C** as an ESP32-S3
  **USB-Serial/JTAG** device: `VID 303A` / `PID 1001`.
- On Windows it enumerates as a CDC COM port (here: `COM9`) plus a
  "USB JTAG/serial debug unit" interface.
- Build runs in WSL2/Linux (fast); **flashing/monitoring is done from Windows**
  directly against the COM port. `usbipd-win` (forwarding the device into WSL2)
  is **not** required and is actually fragile here, because the USB-Serial/JTAG
  device **re-enumerates on every reset**, dropping the usbipd attachment.

## The gotcha: stuck in DOWNLOAD mode (rst:0x15)

Symptom seen during bring-up: after flashing, the serial console was silent and
the ROM banner showed:

```
ESP-ROM:esp32s3-20210327
rst:0x15 (USB_UART_CHIP_RESET),boot:0x0 (DOWNLOAD(USB/UART0))
waiting for download
```

Cause (documented by Espressif): on the USB-Serial/JTAG peripheral, a DTR/RTS
("hard") reset is only a **core reset**, which does **not** re-sample the GPIO0
boot-strapping pin. So once the strap has been sampled LOW (download), the chip
keeps rebooting into download mode instead of SPI boot. Asserting DTR maps to
GPIO0 (active-low), which is why opening the port the "normal" way can also force
download mode.

**Fix:** reset with **`--after watchdog-reset`** (a full system reset that forces
the straps to be re-sampled). Plain `--after hard-reset` (the default, RTS pin)
is for classic USB-UART bridges, not native USB-Serial/JTAG.

## Verified flash command (Windows, simple boot)

A Zephyr simple-boot build is a single ROM-loadable image flashed at offset
`0x0` (no separate bootloader/partition binaries — those are only needed for the
MCUboot/sysbuild path).

```powershell
python -m esptool --chip esp32s3 --port COM9 --baud 921600 `
  --after watchdog-reset write-flash `
  --flash-mode dio --flash-freq 80m --flash-size 8MB `
  0x0 build\zephyr.bin
```

Helper: `scripts/flash_windows.ps1 -Port COM9 -Bin build\zephyr.bin`.

Notes:
- esptool v5 renamed `write_flash` -> `write-flash` (underscore form still works
  with a deprecation warning).
- `--flash-size detect` is the foolproof alternative to `8MB`.
- On Linux with the device attached, `west flash` injects the correct chip,
  offset and flash settings automatically.

## Verified monitor (Windows)

```powershell
python -m esp_idf_monitor --port COM9
```

Helper: `scripts/monitor_windows.ps1`. Keys: exit `Ctrl+]`, reset target
`Ctrl+T Ctrl+R`, help `Ctrl+T Ctrl+H`. The COM number can change after a reset.

`esp-idf-monitor` needs a real TTY. For **headless/automated** capture on Windows
(used to produce `evidence/`), wrap it in a pseudo-console:
`python scripts/monitor_capture_windows.py COM9 15 out.log` (uses `pywinpty`).
The chip must already be in run mode (flash with `--after watchdog-reset` first).

## What this proved on hardware (2026-05-30)

`evidence/20260530-0432-serial.log`:

```
M5StickS3 alive uptime_ms=4162 imu=ready
...
M5StickS3 alive uptime_ms=14163 imu=ready
```

- Boot + USB console work; stable, no reset loop.
- `imu=ready` => BMI270 init succeeded over I2C0 (G47/G48) at 0x68.

## Sources

- esptool Troubleshooting (USB-Serial/JTAG stuck in download; watchdog-reset fix):
  https://docs.espressif.com/projects/esptool/en/latest/esp32s3/troubleshooting.html
- esptool Advanced Options (`--after watchdog-reset`, `--before usb-reset`):
  https://docs.espressif.com/projects/esptool/en/latest/esp32s3/esptool/advanced-options.html
- esptool Boot Mode Selection (DTR->GPIO0, RTS->EN, active-low):
  https://docs.espressif.com/projects/esptool/en/latest/esp32s3/advanced-topics/boot-mode-selection.html
- esptool v5 migration (write_flash -> write-flash):
  https://docs.espressif.com/projects/esptool/en/latest/esp32s3/migration-guide.html
- ESP-IDF USB Serial/JTAG console:
  https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/usb-serial-jtag-console.html
- Zephyr ESP32-S3-DevKitC (simple boot, west flash/monitor):
  https://docs.zephyrproject.org/latest/boards/espressif/esp32s3_devkitc/doc/index.html
- Espressif DevKits with WSL2 (usbipd caveats):
  https://developer.espressif.com/blog/espressif-devkits-with-wsl2/
