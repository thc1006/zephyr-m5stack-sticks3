<#
.SYNOPSIS
  Flash a Zephyr simple-boot image to M5Stack StickS3 from Windows.

.DESCRIPTION
  The StickS3 connects over its native USB-C as an ESP32-S3 USB-Serial/JTAG
  device (VID 303A / PID 1001). On this peripheral a normal RTS/"hard" reset is
  only a CPU core reset and does NOT re-sample the GPIO0 boot strap, so the chip
  gets stuck in download mode (rst:0x15 USB_UART_CHIP_RESET, boot:0x0 DOWNLOAD).
  The documented fix is --after watchdog-reset, which forces a full system reset
  so the straps are re-sampled and the app boots. See
  docs/10_HARDWARE_FLASHING_NOTES.md (sources cited there).

  Requires esptool (pip install esptool). Build the image in WSL/Linux with
  scripts/build_m5sticks3.sh, then copy build/zephyr/zephyr.bin somewhere this
  script can read it (default: <repo>/build/zephyr.bin).
#>
param(
    [string]$Port = "COM9",
    [string]$Bin  = "$PSScriptRoot\..\build\zephyr.bin"
)
$ErrorActionPreference = "Stop"

if (-not (Test-Path $Bin)) {
    throw "Image not found: $Bin (build with scripts/build_m5sticks3.sh and copy zephyr.bin here)"
}

python -m esptool --chip esp32s3 --port $Port --baud 921600 `
    --after watchdog-reset write-flash `
    --flash-mode dio --flash-freq 80m --flash-size 8MB `
    0x0 $Bin
