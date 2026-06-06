# HW-016e — PAGE_AUDIO live mic meter (issue #6), HW-verified

Date: 2026-06-06
Board: m5stack_sticks3/esp32s3/procpu (M5StickS3)
Build: demo (overlay-ble + overlay-audio + overlay-ir), `CONFIG_APP_AUDIO`

## Result: PASS

The shipping on-screen meter tracks sound **live**. Speaking into the on-board
mic drove the AUDIO-page bar to `[####]` with `rms=20680`; it falls back to a low
bar / RMS when quiet. The AUDIO header and text stay clean, and the device stays
responsive on the page (boots clean, stays alive on the AUDIO page at the faster
tick; no freeze observed in testing).

Canonical photo: `evidence/PXL_20260606_032212870.MP~2.jpg`
```
AUDIO
speak now
[####]
rms=20680
```

Boot serial (clean run-mode reset):
```
*** Booting Zephyr OS build v4.4.0 ***
[inf] gfx: Display ready (135x240)
[inf] audio: audio_init OK (16 kHz/16-bit, amp off)
[inf] ble: BLE advertising as "StickS3"
[inf] ir: ir_init OK
alive uptime_ms=... page=3 ...   (page 3 = AUDIO; loop stays alive on the page,
alive uptime_ms=... page=3 ...    ticking ~250 ms; no FATAL / stack fault)
```

## How it works

A dedicated capture thread (`audio_capture_thread` in `app/src/audio.c`) is
enabled only while the AUDIO page is up (`audio_capture_set()` from `main.c`). It
runs ONE continuous full-duplex I2S session (amp OFF, silent TX) and publishes the
peak RMS of each ~128 ms window to `mic_rms_peak`. The UI thread only READS that
level and draws a 0..4 bar via `audio_level_bars()`; the UI never touches I2S.

## Why a thread (the HW-016e finding)

The first attempt drove the meter from the UI render path: each render did a
full-duplex `audio_mic_sample()`, then drew the bar. On hardware that FAILED two
ways:
1. The SPI display write immediately after a full-duplex capture FAILED — the
   AUDIO body rendered all black (header, drawn before the capture, still showed).
2. Run every UI tick, it eventually WEDGED I2S — device frozen, buttons dead, no
   serial output.

The mic itself was never the problem (HW-016d already proved capture works,
rms 0→30797 on a clap). It was a capture↔display interaction. Moving the capture
into its own continuous-streaming thread, with the UI only reading the level,
keeps the SPI display stable.

## On-HW review fixes (after the thread)

1. **Entry ordering** — enable capture only AFTER the AUDIO page is painted (and
   disable it before any other page is painted) so a page's first paint is never
   concurrent with the I2S session start/stop. This cleared residual header
   garbling seen in `evidence/PXL_20260606_013018181~2.jpg`.
2. **Read once** — read `mic_rms_peak` a single time per render so the bar and the
   printed RMS always agree (an async thread update between two reads briefly
   showed `[####]` next to a small RMS).
3. **Two-line layout** — the 10 px font fits only ~13 chars on the 135 px panel, so
   `[####] rms=NNNNN` clipped the RMS to ~2 digits; bar and RMS now sit on separate
   lines (`rms=20680` shows in full).
4. **Adversarial review before flashing** — `ready` and the published level made
   `volatile` (cross-thread reads), capture-thread stack sized for the I2S/codec
   call depth.

## Evidence trail (photos)

- `PXL_20260606_010741580~2.jpg` — first working meter (quiet room, `rms=59`,
  one bar).
- `PXL_20260606_013018181~2.jpg` — capture-thread, pre-layout-fix: `[####]` with a
  clipped `rms=35` and a slightly garbled header.
- `PXL_20260606_032212870.MP~2.jpg` — FINAL: `[####]` `rms=20680`, clean header,
  two-line layout.

See also `evidence/20260606-hw016d-mic-works.log` (the underlying mic-capture
proof) and `docs/03_TDD.md` (HW-016d / HW-016e).
