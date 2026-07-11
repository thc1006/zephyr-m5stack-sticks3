# Issue #7 — ES8311 upstream readiness checklist

Tracks the prep for contributing `drivers/audio/es8311.c` (+ binding + ztest)
to Zephyr upstream. **Strategy as of 2026-07-11: GO — submit our own clean, split
PR.** ADR 0004 originally said "do not compete, engage the live upstream effort".
That effort was abandoned, so ADR 0004's own "fresh clean-split PR" fallback
applies (see its 2026-07-11 update). This file is the "ready to fire" checklist.

## Gate status (live, re-checked 2026-07-11)

Re-check any time with `bash scripts/check_es8311_upstream_gate.sh`.

| Upstream PR | What | State | Effect on us |
|---|---|---|---|
| zephyr#107655 | `boards: espressif: ESP32-S3-BOX-3` (base board) | **MERGED 2026-06-11 21:14Z** | an in-tree consumer now exists: its own doc lists "Speaker with ES8311 audio codec" |
| zephyr#107660 | `drivers: audio: ES8311 + BOX-3 speaker sample` (nnSiD) | **CLOSED 2026-06-12 06:55Z**, never merged | the effort ADR 0004 planned to engage no longer exists |
| zephyr#108073 | `Drivers: audio: add ES8311 codec driver support` (nnSiD) | CLOSED 2026-04-28, never merged | his earlier ES8311 attempt, also abandoned |
| zephyr#110205 | `boards: m5stack: add M5Stack StickS3` (ours) | OPEN, REVIEW_REQUIRED | not a gate; our board lands on its own track |

**Upstream `main` has no ES8311 support at all.** A code search for `es8311` across
the whole tree returns exactly one hit, and it is prose in
`boards/espressif/esp32s3_box3/doc/index.rst`. No driver, no `everest,es8311`
binding, no Kconfig, no DT node.

**Nobody else is driving one.** Codec work upstream is active — #106212 adds a
WM8960, #98902 a TI TAA3020, #112540/#111219 extend wm8904, #110982 tlv320dac310x —
so new codec drivers are welcome, but none of it is ES8311.

**Verdict: GO.** The base board landed, nobody upstream is driving ES8311, and an
in-tree board documents a codec the tree cannot drive. Submit our own focused PR
(codec + binding + ztest; no board, no sample).

Two things to watch while the PR is open:

- **#98500** proposes a `get_caps` API across the DMIC / I2S / Codec subsystems. If
  it lands first, our driver will need to implement it. Not a blocker, but rebase
  onto it rather than fight it.
- Set expectations on pace: #98500, #98902 and #106212 are all low PR numbers and
  still open, so codec-area review can take months. Reviewers on #107660 asked
  repeatedly for the work to be **split** and for samples to be **dropped** —
  submitting codec + binding + ztest only is exactly the shape they were steering
  toward.

## Readiness — re-verified 2026-07-12 against the rewritten driver

- [x] **Driver rewritten against the ES8311 user guide (rev 1.11).** The register
      *values* were always hardware-correct, but several *comments* were not:
      0x03/0x04 hold oversampling rates and 0x05 holds the ADC/DAC dividers, the
      other way round from what the code said; 0x06/0x07/0x08 are inactive in
      slave mode; 0x13 picks the headphone path rather than a drive strength;
      0x15 contains no OSR. All corrected. A reviewer holding the datasheet would
      have caught every one of them.
- [x] **Input volume and mute implemented.** `AUDIO_PROPERTY_INPUT_VOLUME` maps to
      the ADC digital volume (0x17) and `AUDIO_PROPERTY_INPUT_MUTE` to the ADC
      serial port's own mute bit (0x0A bit 6), which is a real mute rather than
      the -95.5 dB volume floor. The `TODO(#7)` in the source is gone.
- [x] **Sample rates 8 kHz to 48 kHz.** The master clock is derived from BCLK, so
      it is 256 * Fs at every rate and the divider chain is a pure ratio: one
      register set serves them all. Corroborated by the vendor clock table, by
      Linux `sound/soc/codecs/es8311.c` (identical REG02 = 0x18 / REG05 = 0x00 at
      every rate), and by `i2s_esp32.c` producing BCLK = 32 * Fs exactly. Word
      sizes other than 16 bits break the ratio and are now rejected instead of
      silently mis-clocking the codec.
- [x] **checkpatch clean** — 0 lines over 100 columns, pure ASCII.
      The 2026-06-11 entry that claimed this was **invalidated in the same week**:
      commit `4fa40be` introduced a 137-column line that would have failed upstream
      CI, and the checklist went on saying "clean". Fixed, and worth remembering:
      a readiness tick is only true for the commit it was taken against.
- [x] **Unit tests** — `tests/drivers/audio/es8311` → twister native_sim **24/24**
      (was 11), covering every supported rate, the rejected rates and word sizes,
      the MCLK validation, the input volume/mute round trip, volume clamping and
      I2C error propagation.
- [x] **Builds against upstream main, zero warnings** — through
      `tests/drivers/build_all/audio`: `CONFIG_AUDIO_CODEC_ES8311=y`, `es8311.c.obj`
      on disk, and the device instance in the ELF symbol table.
- [x] **Genericized** — zero hits for M5Stack / StickS3 / HW-0xx / ESP-ADF /
      esp-bsp / M5GFX / TODO in the driver.
- [x] **DCO ready** — `Signed-off-by: Hsiu-Chi Tsai <hctsai@linux.com>`.
- [x] **No AI footers.**
- [ ] **Hardware validation of the full rate sweep (HW-019) — PENDING.** Only
      16 kHz is hardware-verified today. `CONFIG_APP_AUDIO_RATE_SWEEP` reprograms
      I2S and the codec at each supported rate, reads the clock registers back off
      the chip over I2C, and plays a tone while capturing it on the microphone.
      **8 kHz is the one rate at risk**: both Espressif reference drivers
      special-case it (x4 instead of x8) claiming BCLK has to be at least 512 kHz.
      The datasheet does not say that, Linux contradicts it, and Espressif's own
      11.025 kHz row (x8 at 352.8 kHz) contradicts it too. If it fails on hardware,
      drop `8000` from `es8311_rates[]` before submitting.

## What upstream actually requires (checked against origin/main, 2026-07-12)

MUST:

- `drivers/audio/es8311.c` and `drivers/audio/Kconfig.es8311`.
- One `source` line in `drivers/audio/Kconfig` and one
  `zephyr_library_sources_ifdef` in `drivers/audio/CMakeLists.txt`, both inside the
  `zephyr-keep-sorted` blocks. The ordering is CI-enforced: after `da7212`, before
  `max98091`.
- `dts/bindings/audio/everest,es8311.yaml`.
- A node in `tests/drivers/build_all/audio/i2c_devices.overlay`. That build-only
  overlay is the only test any in-tree codec ships, and it is the only thing that
  makes CI compile the driver at all.
- `static DEVICE_API(audio_codec, es8311_api) = {...}`. The 4.4.0 form
  (`struct audio_codec_api`) became a deprecated alias on main in #110631, so the
  in-repo copy and the upstream copy differ on exactly that one line.

DO NOT:

- Touch `dts/bindings/vendor-prefixes.txt`: `everest` is already there (line 246),
  and a duplicate line is an instant CI failure.
- Touch `MAINTAINERS.yml`: the `Drivers: Audio` area already covers
  `drivers/audio/`, and none of the last four codec PRs touched it. Note that area
  is `status: odd fixes` with **no maintainer**, only collaborators, so budget for
  a slow review.
- Add a release-note entry: those are bulk-added by the release engineer at feature
  freeze, not by the driver PR.
- Ship the ztest and the emulator in the first PR. There is no codec test anywhere
  under `tests/drivers/audio/` and no codec emulator in the tree, so it is new
  surface in an area with no maintainer. Offer it, land it as a follow-up.

Do not claim in the PR description that the driver implements `route_input()` or a
direction-aware `start()`/`stop()`. It implements neither, and an earlier draft of
this document and of ADR 0004 said it did. Capture is enabled by
`configure(AUDIO_ROUTE_CAPTURE)` and is on from then on, which is exactly what the
in-tree wm8904 and da7212 do. The capture support is real; it is `configure()` plus
input volume and mute.

### 3. Submission mechanics
- Branch off current `zephyrproject-rtos/zephyr` main.
- Re-author commits with DCO sign-off, no AI footers, Zephyr commit-message
  style (`drivers: audio: add ES8311 ...`).
- Run `scripts/ci/check_compliance.py` (Kconfig, DTS, Gitlint, Identity) in
  addition to checkpatch.
- Consider a `MAINTAINERS.yml` entry (or let the maintainer add one).
- Lead with the **capture route** as the value-add over #107660; offer a
  real-hardware StickS3 data point.

### 4. Coordinate, don't duplicate
Per ADR 0004: engage #107660 / its successor with a trimmed comment (mute-reg
0x31 confirmation + offer a hardware check). Do not take over the PR uninvited.

## References
- ADR 0004 — `docs/adr/0004-es8311-upstream-engage-pr107660.md`
- Upstream plan item 5 — `docs/07_UPSTREAM_PLAN.md`
- Driver / binding / test — `drivers/audio/es8311.c`, `dts/bindings/audio/everest,es8311.yaml`, `tests/drivers/audio/es8311/`
