<!--
Suggested GitHub labels: Enhancement, area: Audio, platform: ESP32
gh issue create --title "M5StickS3: ES8311 audio codec + I2S" \
  --body-file docs/issues/0003-enh-es8311-audio.md --label "Enhancement"
-->
# Enhancement: M5StickS3 ES8311 audio codec + I2S

**Is your enhancement proposal related to a problem? Please describe.**
The StickS3 has an Everest ES8311 audio codec (I2C 0x18), a MEMS microphone and
a speaker amplifier, none of which are supported by the current board port.

**Describe the solution you'd like**
- Check ES8311 codec support status in upstream Zephyr (driver/binding).
- Wire the I2S pins (MCLK G18, DOUT G14, BCLK G17, LRCK G15, DIN G16) via
  pinctrl + an I2S node, plus the ES8311 on I2C 0x18.
- Enable the speaker-amp rail/enable (M5PM1 PYG3) — depends on issue 0001.
- First milestone: codec register read (chip-id) proof; then I2S loopback or a
  single-sample-rate playback.

**Describe alternatives you've considered**
PWM "beeper" output only (insufficient — does not exercise the codec/mic).

**Additional context**
- Out of scope for v0.1 (documented as roadmap in `docs/02_SDD.md`).
- Speaker-amp vs IR-RX conflict (see issue 0002).

**Acceptance criteria**
- ES8311 register access succeeds; I2S playback or loopback works at one rate.
- Evidence (serial log + audio capture/photo) saved under `evidence/`.

**Status (2026-06-06): DONE.** Tracked in this repo as GitHub issue #6.
ES8311 register access + 16 kHz / 16-bit I2S playback are HW-verified (HW-006,
440 Hz beep audible), and ADC mic capture is HW-verified (HW-016d): a loud
external sound drives the on-board mic -> ES8311 ADC -> I2S0 RX -> DSP chain from
~0 to full scale, and PAGE_AUDIO shows a live mic-level meter. The long-standing
"capture silent (rms=0)" reading was a weak-acoustic-stimulus confound (the tiny
on-board beep barely couples to the mic), not a mic/ADC fault. Evidence under
`evidence/`: HW-006 boot log, `20260606-hw016d-mic-works.log`, live-meter log/photo.
