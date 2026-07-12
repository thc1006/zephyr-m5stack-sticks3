"""Headless serial capture for the M5StickS3 console on Windows.

esp-idf-monitor needs a real TTY; this wraps it in a Windows pseudo-console
(ConPTY via pywinpty) so it can be run non-interactively to produce the logs in
evidence/. Flash with `esptool --after watchdog-reset` first so the chip is in
run mode.

By default it connects with --no-reset, which does not disturb the running app.
That is right for watching something a person is driving from the buttons, and
WRONG for anything that happens once at boot: by the time the monitor has
attached, it is over and the log is empty of it. Pass --reset and the monitor
resets the chip on connect, so the capture starts at the banner.

    --reset is required for the ES8311 rate sweep (HW-019) and the PSRAM
    self-test (HW-020). Both run early in main and never run again.

Usage:  python scripts/monitor_capture_windows.py COM9 30 out.log [zephyr.elf] [--reset]
Requires: pip install esp-idf-monitor pywinpty
"""
import sys
import time

from winpty import PtyProcess

args = [a for a in sys.argv[1:] if a != "--reset"]
RESET = "--reset" in sys.argv[1:]

PORT = args[0] if len(args) > 0 else "COM9"
SECS = float(args[1]) if len(args) > 1 else 15.0
LOG = args[2] if len(args) > 2 else "monitor.log"
ELF = args[3] if len(args) > 3 else ""

cmd = "python -m esp_idf_monitor"
if not RESET:
    cmd += " --no-reset"
cmd += " --port %s" % PORT
if ELF:
    cmd += " %s" % ELF
print("monitoring %s for %.0fs (%s)" % (PORT, SECS, "reset on connect" if RESET
                                        else "no reset: a boot-time log will be MISSED"))

logf = open(LOG, "w", encoding="utf-8", errors="replace")
proc = PtyProcess.spawn(cmd, dimensions=(40, 120))

# PtyProcess.read() BLOCKS until data arrives, so a device that goes silent -- which
# is exactly what a firmware hang looks like -- wedges this loop forever and the
# deadline above is never re-checked. That happened on HW-019b: the sweep stopped
# printing mid-run, the monitor never returned, and it held the COM port until it was
# killed by hand. Read on a worker thread and let the main loop own the clock.
import threading

try:
    import queue
except ImportError:  # pragma: no cover - py2
    import Queue as queue

q = queue.Queue()


def _pump():
    while True:
        try:
            d = proc.read(4096)
        except Exception:
            break
        if not d:
            break
        q.put(d)


threading.Thread(target=_pump, daemon=True).start()

end = time.time() + SECS
total = 0
quiet = 0.0
while time.time() < end:
    try:
        data = q.get(timeout=0.5)
    except queue.Empty:
        quiet += 0.5
        continue
    quiet = 0.0
    total += len(data)
    logf.write(data)
    logf.flush()
try:
    proc.terminate(force=True)
except Exception:
    pass
logf.write("\n[monitor captured %d chars; last %.1fs silent]\n" % (total, quiet))
logf.close()
print("captured %d chars to %s" % (total, LOG))
