# HW-016 ES8311 audio capture (microphone) bring-up - 2026-06-05

Board: physical M5Stack StickS3 (ESP32-S3-PICO-1, MAC 70:04:1d:db:ab:b8),
Zephyr 4.4.0, Zephyr SDK 1.0.1. Flash + monitor over USB-Serial/JTAG on COM9
(`esptool --after watchdog-reset`, `monitor --no-reset`).

Goal (issue #6): bring up the ES8311 ADC / on-board analog MEMS mic
(MSM381A3729H9BPC) capture path and verify a real captured signal on hardware.

## What runs

The gated `CONFIG_APP_AUDIO` build (`-DEXTRA_CONF_FILE=overlay-audio.conf`).
`audio_loopback()` plays the 440 Hz tone on the speaker (amp on for the beep
window) while capturing the mic over I2S0 RX (`I2S_DIR_BOTH`), and prints the
per-block mono RMS/peak (`audio_dsp.c`: deinterleave the I2S slot, RMS, peak).
A SIL / BEEP / SIL block sequence gives a before/during/after contrast.

A temporary boot harness (in `app/src/main.c`, reverted after this session) ran
`audio_loopback()` a few times spaced 2.5 s apart so each run starts from an
idle device and the MIC lines print headlessly. Note: re-running the loopback
back-to-back with no gap exhausts the TX slab and trips `loopback prequeue
failed`; one run per idle device is clean (the demo only ever runs it once per
page entry, so this is a harness artifact, not a demo bug).

## Two configs (the ADCDAT mux, register 0x44)

1. Shipping config `0x44 = 0x08` (ADCDAT_SEL=0): ASDOUT carries plain ADC data
   (the real analog mic). Evidence: `20260605-hw016-audio-analog-mic.log`.
2. Diagnostic `0x44 = 0x68` (ADCDAT_SEL=DACL+DACR): ASDOUT carries a pure
   digital copy of the DAC, bypassing the analog front end AND the ADC
   modulator. Used only to exercise the RX transport + DSP independently of the
   mic. Reverted to 0x08 after capture. Evidence:
   `20260605-hw016-audio-digital-loopback.log`.

## Result

| Config | SIL rms | BEEP rms / peak | slot-probe |
|---|---|---|---|
| 0x08 analog mic | 0 (peak <= 8) | **0** (peak <= 8) | slot0 rms=0, slot1 rms=0 |
| 0x68 digital | 0 | **4089 / 5800** (bit-stable) | both 0 until tone lands |

- Digital loopback (0x68): the captured stream reads a clean, bit-stable
  rms=4089 / peak=5800 during the beep. This proves the ASDOUT serial output,
  the I2S0 full-duplex RX path, and the capture DSP (deinterleave / RMS / peak)
  all carry data end to end. Note this is the DIGITAL-mux path: 0x68 puts a copy
  of the DAC on ASDOUT, so it proves the serial-out/RX/DSP chain but does NOT
  prove the RX path carries live ADC-domain samples.
- Analog mic (0x08): rms stays at 0 (peak <= 8, i.e. the LSB noise floor; the
  highest beep-block peak was 7) through the beep, on BOTH I2S slots (so it is
  not a wrong-slot pick). With the identical serial-out/RX/DSP chain that just
  carried a perfect 5800-peak digital signal, the ADC capture configuration
  delivers nothing.

## Conclusion (honest scope)

- runtime-verified on hardware: the ES8311 ASDOUT serial output, the I2S0
  full-duplex RX path, and the capture DSP (deinterleave / RMS / peak), via the
  0x68 digital-mux loopback. This is the serial-out -> RX -> DSP chain only.
- NOT working: with `0x44 = 0x08` (real-ADC route) the captured stream is silent
  (rms=0) during the beep. The fault is somewhere on the ADC route, but this
  test does NOT localize it. It is consistent with any of: the analog front end
  (PGA/MIC1/bias), the ADC modulator or its power (0x0D/0x0E) or ADC clock/OSR,
  OR the `0x44 = 0x08` ADCDAT mux itself not routing real ADC data to ASDOUT
  (only the 0x68 routing is proven). The 0x68 test bypasses the ADC modulator
  entirely, so it cannot exonerate it.
- It is also NOT yet separated from the no-stimulus case: the beep audibility
  was not confirmed this headless session (playback audibility is HW-006
  verified, but a self-acoustic loopback assumes the speaker couples into the
  mic, which was not independently checked). With no acoustic stimulus, rms=0
  would be the expected result regardless of ADC health.
- Issue #6 acceptance ("a real captured signal observed on hardware" from the
  analog mic) is NOT met. #6 stays open; the analog-ADC bring-up continues in a
  focused follow-up.

## Next test that would localize the fault

A single `0x44 = 0x58` (ADCL = real ADC, DACR = DAC) capture: it puts real
ADC-left and a known-good DAC-right in the SAME block over the SAME transport.
If DACR-right reads ~5800 while ADCL-left reads 0, that rules out no-stimulus,
wrong-slot, transport, DSP and the mux-routing confound in one shot and pins the
fault to the ADC modulator / power / PGA / bias. Register-level suspects to chase
then: 0x14 single-ended MIC1 + PGA, the mic power/bias rail, 0x0D analog
power/charge-pump ordering, ADC OSR/clock coupling.

## Evidence provenance / DoD gaps (this is an INTERIM note, not a full milestone)

- The two capture logs were produced by a temporary boot harness that called the
  same `audio_loopback()` the AUDIO page runs on entry (only the caller changed,
  boot vs button); the codec/I2S/DSP path and the 0x08 register set are identical
  to the shipping build.
- Build + flash logs for the reverted shipping build are saved alongside
  (`20260605-hw016-audio-build.log`, `20260605-hw016-audio-flash.log`).
- LCD photo of the AUDIO page showing `mic bar=0 rms`:
  `evidence/PXL_20260604_190722170.MP.jpg` (2026-06-04, the same shipping-build
  state on hardware). #6 stays OPEN, so this is an interim record of "mic reads
  zero", not a "capture works" claim.

## Reproduce

```bash
# build (WSL workspace; repo wired in as BOARD_ROOT + ZEPHYR_EXTRA_MODULES)
cd ~/zephyrproject-m5sticks3 && source .venv/bin/activate
export BOARD_ROOT=<repo> ZEPHYR_EXTRA_MODULES=<repo>
west build -p always -b m5stack_sticks3/esp32s3/procpu <repo>/app \
    -d build_audio -- -DEXTRA_CONF_FILE=overlay-audio.conf
```

```powershell
# flash + capture (Windows, COM9)
python -m esptool --chip esp32s3 --port COM9 --baud 921600 \
    --after watchdog-reset write-flash 0x0 build_audio/zephyr/zephyr.bin
python scripts/monitor_capture_windows.py COM9 16 out.log
```
