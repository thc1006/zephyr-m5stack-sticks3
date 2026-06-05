# ADR 0004 — Engage upstream ES8311 PR #107660 instead of a competing driver

## Status

Accepted (2026-06-05). Supersedes the "open our own ES8311 PR" framing of task #21 / issue #7.

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
