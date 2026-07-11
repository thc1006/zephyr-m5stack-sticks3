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

## Readiness — verified 2026-06-11 (all GREEN)

- [x] **checkpatch clean** — `es8311.c`, `emul_es8311.c`, `Kconfig.es8311`,
      `everest,es8311.yaml`, test `main.c` all return 0 errors / 0 warnings under
      Zephyr's `.checkpatch.conf` (the earlier SPDX/EXTERN/CONFIG_DESCRIPTION
      hits were Linux-kernel checks Zephyr disables).
- [x] **Unit tests pass** — `tests/drivers/audio/es8311` → twister native_sim
      11/11 cases pass.
- [x] **DCO ready** — existing commits already carry
      `Signed-off-by: Hsiu-Chi Tsai <hctsai@linux.com>`.
- [x] **No AI footers** — no `Co-Authored-By` / `Generated with` / Claude/
      Anthropic tells in the commit history of these files.
- [x] **Driver code is board-independent** — `DT_DRV_COMPAT everest_es8311`,
      generic `audio_codec_api` registration, no `#ifdef`/hardcoded M5Stack logic.
- [x] **Capture/ADC route present** — the differentiator vs #107660 (which
      disables ADC). Zephyr's codec API already models capture
      (`route_input`/direction-aware start/stop).
- [x] **Tree layout matches upstream** — `drivers/audio/`, `dts/bindings/audio/`,
      `tests/drivers/audio/es8311/` mirror the Zephyr structure.

## TODO at submission time (gated — do when the window opens)

### 1. Genericize board/project-specific references (the only code cleanup)
The driver *logic* is generic, but comments/description name our board and
internal HW-IDs. For the upstream copy, rephrase to keep the technical content
and drop project-internal tells. Keep Espressif/ESP-ADF/esp-bsp provenance
(those strengthen the contribution).

- `drivers/audio/es8311.c`: lines ~10, 14–15, 73, 100–101, 136, 154, 160, 172,
  323, 422 — replace "M5Stack StickS3 / this project's bring-up / HW-016/HW-016d"
  with generic phrasing ("a 16 kHz / 16-bit MCLK-from-BCLK configuration",
  "hardware-validated").
- `dts/bindings/audio/everest,es8311.yaml`: lines 9, 13 — drop the StickS3
  sentence from `description`.
- `drivers/audio/Kconfig.es8311`: help text — drop "Used on the M5Stack StickS3".

### 2. Split into a focused PR (reviewer norm on #107660)
Codec driver + binding + ztest only. No board, no sample in the same PR.

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
