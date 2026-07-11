# ADR 0004 — Engage upstream ES8311 PR #107660 instead of a competing driver

## Status

Accepted (2026-06-05). Superseded the "open our own ES8311 PR" framing of task #21 /
issue #7.

**Amended 2026-07-11 — the decision is now GO.** The upstream effort this ADR chose
to engage was abandoned (every ES8311 PR closed, none merged), which triggers this
ADR's own documented fallback: open a fresh clean-split PR. Read the
"Update 2026-07-11" section at the bottom before acting on anything below it.

## Context

We have an independently written ES8311 codec driver (`drivers/audio/es8311.c`):
playback HW-verified on the StickS3 (HW-006), an ADC/capture route added, native
ztest 11/11, plus a mute-bit fix at reg 0x31 bits[6:5] (DSMMUTE|DEMMUTE, mask
0x9F set 0x60). Issue #7 asks how to bring ES8311 upstream.

Deep research (2026-06-05, multi-source, adversarially verified) found:

- There is **no merged ES8311 driver** and no `everest,es8311` binding in the
  Zephyr tree (main, June 2026).
- The sole live upstreaming vehicle is **PR #107660** (author nnSiD), OPEN but
  **NOT mergeable** (`mergeStateStatus DIRTY` / `mergeable CONFLICTING`), with two
  `CHANGES_REQUESTED` reviews: **marekmatej** (2026-04-22) and **MaureenHelm**
  (MEMBER, 2026-05-11). Reviewers asked the author to trim the description, SPLIT
  the work into separate PRs (codec vs board/sensor vs sample), move the sample
  to `src/`, use `CONFIG_HEAP_MEM_POOL_ADD_SIZE_*`, drop conditional CMake checks,
  and fix failing CI. A sibling PR #108073 was self-closed 2026-04-28.
- #107660 is **playback only** (495-line `es8311.c`, volume + DAC mute, the ADC
  path is actively disabled: "Enable DAC path, disable ADC path for playback").
- The current Zephyr audio codec API on main **does** support capture
  (`AUDIO_ROUTE_CAPTURE`/`PLAYBACK_CAPTURE`, `route_input`/`route_output`,
  direction-aware start/stop). So our HW-verified capture/ADC route is an
  API-fit differentiator.
- Our 0x31 mute fix matches #107660's (both mask 0x9F set 0x60), corroborated by
  ESP-ADF / the Everest datasheet / Linux `sound/soc/codecs/es8311` — so the mute
  is NOT a differentiator; the capture route is.
- Zephyr norms: small self-contained PRs, search-the-tracker-first,
  coordinate-don't-duplicate, and a documented stalled-PR escalation (ping
  reviewers after ~1 week, ask in #pr-help after ~2 weeks).

## Decision

Do **not** open a competing or alternative ES8311 driver. Engage PR #107660:

1. Post HW-verified feedback as a real-hardware data point: we run an ES8311 on a
   physical M5Stack StickS3 with playback HW-verified, and the binding/register
   set we use agrees with #107660's on the load-bearing points (mute 0x31, the
   codec-API shape).
2. Offer the **capture/ADC route** as a clean follow-up once #107660 lands, since
   the codec API already models it and #107660 omits it. Do not push it into
   #107660 now (the reviewers want the PR split down, not grown).
3. Leave the reviewer asks (split, conflicts, CI) to the author; offer help, do
   not take over the PR uninvited.

Our in-repo `drivers/audio/es8311.c` stays repo-local until #107660 (or a
successor) merges, then we contribute the capture route on top.

## Consequences

Positive:

- Non-duplicative and mission-aligned; helps the existing effort land rather than
  fragmenting it.
- Positions our HW-verified capture route as the natural next contribution.
- A real-board ES8311 data point is rare and useful to maintainers.

Negative:

- Progress depends on #107660's author resolving conflicts / the split (outside
  our control); if it goes fully stale, revisit (revive with permission, or a
  fresh clean-split PR after coordinating on the tracker).
- Our capture route stays unmerged in the meantime.

## Update 2026-06-05 — DO NOT engage #107660 yet (hold)

A pre-post review (verified against the live tracker) changed the timing of the
decision above: **do not post on #107660 now.**

- The author (nnSiD) said on the thread (2026-05-14) "we've fixed these issues in
  the new PR. Kindly check." No PR number was given in that comment (a "#108078"
  cited in an earlier draft of this ADR was wrong: #108078 is an unrelated AT581X
  radar PR). So the author has shifted effort off #107660, but the successor PR is
  not named in the thread.
- Maintainer **marekmatej asked the author to focus on the base board PR #107655
  first** ("to speed up reviewing and merging, please focus on #107655. With base
  support up and running, adding new features is more straightforward").
  #107655 (ESP32-S3-BOX-3 board) is OPEN/BLOCKED and actively moving.
- The thread is freshly sensitive to outside pressure (a "when will [this be]
  merged?" pile-on comment), so an unsolicited "I have more code to contribute"
  note on a stalled, superseded, deprioritized PR would read as scope-creep.

Revised plan: **hold.** Wait for the base board (#107655) to land and the ES8311
work to resume on a live PR; engage there with a **trimmed** comment that drops
the self-contribution offer and leads only with (a) the mute-register
confirmation and (b) an offer to run a hardware check on a real StickS3. The
prepared comment (technically verified, AI-tell-free) is parked, not posted.

## Update 2026-07-11 — the hold is released: there is nothing left to engage

Both the decision above and the 2026-06-05 hold assume a *live* upstream ES8311
effort to coordinate with. Checked against live GitHub on 2026-07-11 (via
`scripts/check_es8311_upstream_gate.sh`), that assumption is dead:

- **The base board #107655 MERGED** on 2026-06-11T21:14:01Z. That was the one thing
  marekmatej had asked the author to land first.
- **The next day the author's remaining PRs were closed.** #107660 (ES8311 codec +
  BOX-3 speaker sample) was closed 2026-06-12T06:55:18Z and #108078 (his AT581X radar
  PR — the "new PR" he had pointed MaureenHelm at on 2026-05-14, which in fact
  contains no ES8311 code at all) three minutes later at 06:58:36Z. His earlier ES8311
  attempt #108073 had already been closed on 2026-04-28, and the ES7210 PR #107661 on
  2026-05-29. The base board is in; every peripheral PR is closed and none merged.
- **Upstream `main` has no ES8311 support at all.** A code search for `es8311` across
  the whole tree returns exactly one hit, and it is prose in
  `boards/espressif/esp32s3_box3/doc/index.rst`: "Speaker with ES8311 audio codec".
  No driver, no `everest,es8311` binding, no Kconfig, no DT node. `drivers/audio/`
  holds aw88298, cs43l22, wm8904, tlv320 and friends, and no es8311.
- **Nobody else is driving one.** The only open ES8311 PR upstream is our own board
  PR #110205, which merely names the part. Other codec work *is* in flight (#106212
  adds a WM8960 driver, #98902 a TI TAA3020, #112540/#111219 extend wm8904, #110982
  tlv320dac310x, and #98500 proposes a `get_caps` API across the DMIC/I2S/Codec
  subsystems) — so new codec drivers are welcome and the area is active — but none
  of it touches ES8311.

So upstream now ships an in-tree board whose own documentation advertises an ES8311
speaker path, with nothing in the tree able to drive it, and the only person who
tried has withdrawn every attempt. The "do not fragment a live effort" reason for
holding no longer has a live effort to protect. This is precisely the fallback this
ADR already recorded under Consequences / Negative: *"if it goes fully stale, revisit
(revive with permission, or a fresh clean-split PR after coordinating on the
tracker)"*. Nothing was ever posted on #107660, so no coordination is being
withdrawn, and the parked drafts in `docs/drafts/es8311-upstream-comments.md` are
moot.

### Decision: GO

Submit our own PR: the codec driver + `Kconfig.es8311` + the `everest,es8311`
binding + a node in `tests/drivers/build_all/audio/i2c_devices.overlay`, with no
board and no sample, per `docs/issues/0007-es8311-upstream-readiness.md`. As a
courtesy, leave a short note on the closed #107660 so the previous author and its
reviewers see it coming.

**Amended 2026-07-12, two scope corrections found while preparing the submission:**

- **The ztest and the emulator stay out of the first PR.** There is no codec test
  anywhere under `tests/drivers/audio/` and no codec emulator in the tree, so both
  are new surface in an area that has no maintainer, only collaborators. Offer them
  and land them as a follow-up; the build-only overlay is what every other codec
  ships and it is what makes CI compile the driver.
- **Do not sell the capture route as `route_input()` / direction-aware
  `start()`/`stop()` API fit.** The Context section above says the API models
  capture that way, which is true of the API but not of our driver: it implements
  neither callback. Capture is enabled by `configure(AUDIO_ROUTE_CAPTURE)` and is on
  from then on, as in the in-tree wm8904 and da7212. The differentiator is real, but
  it is `configure()` plus `AUDIO_PROPERTY_INPUT_VOLUME` and
  `AUDIO_PROPERTY_INPUT_MUTE`, hardware-verified at HW-016d. Claiming callbacks we
  do not implement would be caught in the first review pass.

### Consequence for the gate script

`scripts/check_es8311_upstream_gate.sh` required a live *successor* PR to appear
before it would open. That deadlocks: once an effort is abandoned no successor ever
appears, so the gate could never open, and "nobody is doing it" would be read as a
reason to keep waiting rather than a reason to act. It now reports GO / ENGAGE /
HOLD. Inverting that condition introduced a mirror of the original false-merge bug —
a failed `gh search` looks exactly like "nobody is driving it" — so the search must
now *succeed* to count, and any gh failure still degrades to HOLD. The offline test
covers all six cases, including both fail-open traps.
