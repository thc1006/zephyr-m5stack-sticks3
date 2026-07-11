# ES8311 upstream — parked comment drafts (issue #7)

Status: **DRAFTS — not posted.** Hold per ADR 0004 Update (2026-06-05).
Last live-tracker check: 2026-06-11 — #107660 still DIRTY/CONFLICTING (no movement
since 2026-06-05); #107655 base board OPEN/BLOCKED (moving, not merged); no named
successor ES8311 PR. Hold conditions still hold.

Trigger to post Draft B: (a) base board #107655 merges AND (b) ES8311 work resumes
on a live PR (#107660 de-conflicts, or a named successor appears).

---

## Draft A — framing note for issue #7 (thc1006/zephyr-m5stack-sticks3)

Post when convenient (low sensitivity; our own tracker). Aligns the issue with ADR 0004.

> Updating the plan to match ADR 0004. Upstream already has a live ES8311 effort
> (zephyrproject-rtos/zephyr#107660), so instead of opening a competing driver we'll
> engage that PR and contribute our differentiator — the hardware-verified
> capture/ADC route, which #107660 currently omits — as a clean follow-up once the
> playback codec lands. Holding until the base board PR #107655 merges and the
> ES8311 work resumes on a live PR. The in-repo driver stays repo-local meanwhile
> (native_sim ztest 11/11; playback HW-006 and capture HW-016d verified on a real
> StickS3).

---

## Draft B — engagement comment for the live upstream ES8311 PR

Post ONLY after the trigger above. Leads with the mute-register confirmation and a
hardware-check offer; deliberately drops any "we have more code to contribute".

> We're running an Everest ES8311 on a physical M5Stack StickS3 (ESP32-S3) under a
> Zephyr port, and can confirm one load-bearing detail from real hardware: muting
> the DAC at register 0x31 needs both DSMMUTE and DEMMUTE (bits [6:5]) — mask 0x9F,
> set 0x60 — to go cleanly silent. That matches what this PR does and agrees with
> the Everest datasheet, ESP-ADF, and the Linux `sound/soc/codecs/es8311` path.
> Playback at 16 kHz / 16-bit is audible on our board.
>
> If it helps review confidence, we're happy to run a hardware smoke-test of this
> driver on a real StickS3 (chip-id probe + playback) and report back — a second
> physical board besides the S3-BOX-3. Let us know.

---

## Submission-time work (gated)

The full "ready to fire" checklist lives in
`docs/issues/0007-es8311-upstream-readiness.md` — see it for the genericize list,
the split-PR scope, and the submission mechanics. Verified 2026-06-11:

- checkpatch under Zephyr's `.checkpatch.conf` (the CI config) → **0 errors,
  0 warnings** on driver, emul, binding, Kconfig and test. (A bare `--no-tree`
  run reports SPDX-line / extern nits, but those are Linux-kernel checks Zephyr
  disables, so they are not real findings.)
- twister native_sim → 11/11 pass.
- The only real pre-submission task is genericizing the M5Stack/HW-ID references
  in comments (logic is already board-independent) — itemized in the readiness doc.
