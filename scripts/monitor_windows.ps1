<#
.SYNOPSIS
  Open the M5Stack StickS3 serial console from Windows.

.DESCRIPTION
  Uses esp-idf-monitor against the ESP32-S3 USB-Serial/JTAG COM port. Run this
  in an interactive terminal (it needs a real TTY). Exit with Ctrl+], reset the
  target with Ctrl+T Ctrl+R, help with Ctrl+T Ctrl+H.

  Note: after a reset/flash the COM port may re-enumerate (number can change).
  Requires: pip install esp-idf-monitor.
#>
param(
    [string]$Port = "COM9"
)
$ErrorActionPreference = "Stop"

python -m esp_idf_monitor --port $Port
