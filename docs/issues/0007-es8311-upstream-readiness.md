# Issue #7 — ES8311 upstream readiness checklist

Tracks the prep for contributing `drivers/audio/es8311.c` (+ binding + ztest)
to Zephyr upstream. **Strategy is set by ADR 0004: do NOT open a competing PR;
engage the existing effort and HOLD until the gate opens.** This file is the
"ready to fire" checklist so the actual submission is fast once the gate lifts.

## Gate status (live, re-checked 2026-06-11)

| Upstream PR | What | State | Gates us? |
|---|---|---|---|
| zephyr#110205 | `boards: m5stack: add M5Stack StickS3` (our maintainer thc1006) | **OPEN, APPROVED**, not merged | once merged → clean board-driven path for our codec |
| zephyr#107655 | `boards: espressif: ESP32-S3-BOX-3` (base board) | OPEN, CHANGES_REQUESTED, active (upd 6/10) | ADR gate: ES8311 work resumes after this lands |
| zephyr#107660 | `drivers: audio: ES8311 + BOX-3 speaker sample` (nnSiD) | OPEN, CHANGES_REQUESTED, **stalled since 6/5**, playback-only (ADC disabled) | the existing effort to coordinate with |

**Hold conditions still in force**: neither #107655 nor #110205 has merged, and
no live successor ES8311 PR exists. Do not post upstream yet (ADR 0004 update
2026-06-05).

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
