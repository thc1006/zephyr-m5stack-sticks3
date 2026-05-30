"""Headless serial capture for the M5StickS3 console on Windows.

esp-idf-monitor needs a real TTY; this wraps it in a Windows pseudo-console
(ConPTY via pywinpty) so it can be run non-interactively to produce the logs in
evidence/. Connects with --no-reset (does not disturb the running app); flash
with `esptool --after watchdog-reset` first so the chip is in run mode.

Usage:  python scripts/monitor_capture_windows.py COM9 15 out.log [zephyr.elf]
Requires: pip install esp-idf-monitor pywinpty
"""
import sys
import time

from winpty import PtyProcess

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM9"
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0
LOG = sys.argv[3] if len(sys.argv) > 3 else "monitor.log"
ELF = sys.argv[4] if len(sys.argv) > 4 else ""

cmd = "python -m esp_idf_monitor --no-reset --port %s" % PORT
if ELF:
    cmd += " %s" % ELF

logf = open(LOG, "w", encoding="utf-8", errors="replace")
proc = PtyProcess.spawn(cmd, dimensions=(40, 120))

end = time.time() + SECS
total = 0
while time.time() < end and proc.isalive():
    try:
        data = proc.read(4096)
    except EOFError:
        break
    if data:
        total += len(data)
        logf.write(data)
        logf.flush()
try:
    proc.terminate(force=True)
except Exception:
    pass
logf.write("\n[monitor captured %d chars]\n" % total)
logf.close()
print("captured %d chars to %s" % (total, LOG))
