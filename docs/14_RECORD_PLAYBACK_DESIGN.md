# Issue #14 — Mic record → speaker playback: requirements, baseline & design

Status: **implemented & HW-verified (2026-06-09, dell-0830)**; see
`evidence/20260609-hw014-record-playback.log`. Tracks GitHub issue #14 ("Audio
demo: record a short mic clip and play it back"). Feature branch:
`feature/14-record-playback`.

## 1. Purpose (why this feature exists)

Capture (#6) and playback (#3) are each HW-verified in isolation, but only as a
*level meter* and a *synthesised* 440 Hz tone respectively. Neither proves that
**real captured audio data** survives the full round trip. This feature closes
that gap:

> Record a short spoken phrase to RAM and play it back so it is **recognisable**,
> proving the end-to-end path mic → ES8311 ADC → I2S0 RX → RAM → I2S0 TX →
> ES8311 DAC → AW8737 amp → speaker carries good audio *data*, not just a signal.

## 2. Requirements

### Functional (must do)

| ID | Requirement |
|----|-------------|
| FR-1 | Record a fixed-length clip from the on-board mic into a RAM buffer |
| FR-2 | Store it as 16 kHz / 16-bit mono |
| FR-3 | Play the stored clip back through the speaker |
| FR-4 | Let the user start recording and clearly know **when to speak** |
| FR-5 | Show state (ready / recording / review / playing) on screen + serial |
| FR-6 | Be repeatable without a reboot |

### Quality (how well)

| ID | Requirement | Rationale |
|----|-------------|-----------|
| QR-1 | Played-back speech is **intelligible** | The real acceptance bar — not "a sound came out" |
| QR-2 | Never corrupt the SPI display or wedge I2S | HW-016e rule: the UI thread must never touch I2S |
| QR-3 | Amp anti-pop; speaker muted at rest | Reuse the audio_beep() amp sequencing |
| QR-4 | Clip buffer fits internal SRAM | See §3 baseline (ample headroom) |
| QR-5 | Produce a serial log (+ optional A/V) under `evidence/` | Project evidence/honesty rule |

### Constraints (from platform + codebase)

- Two usable buttons (KEY1 middle / KEY2 right), both normally page-nav.
- Mic is mono on I2S slot 0; codec fixed at 16 kHz / 16-bit; codec output volume
  0 dB with a digital make-up gain (`CONFIG_APP_AUDIO_REC_GAIN_Q8`, Q8) on the
  recorded clip; the on-board speaker is small/quiet.
- Full-duplex shares one clock: TX must keep streaming (silence) or RX stalls.
- Record/playback must run on the audio thread; the UI thread only reads state.

### Agreed decisions

- **A. Clip length:** "at least a short phrase." Baseline (§3) shows ample SRAM,
  so target **5 s** (≈ a short sentence), Kconfig-tunable. 5 s mono = 156 KB.
- **B. Trigger / mode switching:** a modal recorder on the REC page using the two
  buttons (short/long press) — see §4 (proposed, under review).
- **C. Playback:** prioritise **clear, intelligible** replay → apply a tunable
  digital gain to compensate for the quiet speaker + low volume.
- **D. Baseline:** quantify the current state first (§3) before sizing anything.

## 3. Quantified baseline (current state, before any #14 code)

Measured on 2026-06-09, Zephyr 4.4.0, SDK 1.0.1, board
`m5stack_sticks3/esp32s3/procpu`.

### Memory footprint

| Region | Default build | Audio build (`overlay-audio.conf`) | Region size |
|--------|--------------:|-----------------------------------:|------------:|
| FLASH | 142 968 B (1.70 %) | 145 516 B (1.73 %) | 8 388 352 B |
| iram0_0_seg | 48 656 B (11.71 %) | 49 520 B (11.92 %) | 415 492 B |
| dram0_0_seg | 58 936 B (14.77 %) | 77 416 B (19.40 %) | 399 108 B |

**dram0 free in the audio build: 321 692 B (≈ 314 KB).** This is the budget the
record buffer draws from.

### Record-buffer sizing (16 kHz × 16-bit × mono = 32 000 B/s)

| Clip | Buffer | dram0 free after | Verdict |
|------|-------:|-----------------:|---------|
| 1 s | 31 KB | ~283 KB | fits easily |
| 3 s | 94 KB | ~220 KB | fits easily |
| **5 s** | **156 KB** | **~158 KB** | **chosen default** |
| 8 s | 250 KB | ~64 KB | fits, less margin |

### Audio reference numbers (from prior HW-verified work)

- Format: 16 kHz / 16-bit; I2S block = 256 frames = 1024 B = 16 ms; mic mono on
  slot 0 (HW-016d).
- Capture RMS: quiet ~70–100; speech a few hundred to a few thousand; loud clap
  ≥ 12 000 (HW-016d peaked 30 797). PAGE_AUDIO meter full-scale = 2000.
- Digital-mux loopback: rms = 4089 / peak = 5800.
- Tone playback (legacy #3 baseline): amplitude 5800 (~ −15 dBFS), codec volume
  −20 dB. Record→playback (#14) instead runs the codec at 0 dB and makes up level
  digitally via `CONFIG_APP_AUDIO_REC_GAIN_Q8`.

These give concrete targets: e.g. a recorded phrase should show capture RMS in
the hundreds–thousands, and playback should be audible/recognisable.

## 4. Proposed button / mode design (B — under review)

The REC page becomes a **modal recorder**: only on this page the two buttons
change context (every other page is unchanged). Short vs long press lets one
button express two actions, so two buttons cover record / play / re-record /
exit, with the current mapping always drawn on screen.

### State machine

```
 enter REC page
      │
      ▼
  ┌────────┐  KEY1 short   ┌───────────┐  N s elapsed  ┌──────────┐
  │ READY  │ ────────────▶ │ RECORDING │ ────────────▶ │  REVIEW  │
  └────────┘   (record)    └───────────┘   (auto)      └──────────┘
      │                                          │  ▲      │
   KEY2│ exit page                       KEY1    │  │ play │ KEY1 long
      ▼                                   short  ▼  │ done │ (re-record)
   next page                            ┌──────────┐│      ▼
                                        │ PLAYING  │┘  back to RECORDING
                                        └──────────┘
```

### Button map (shown on screen, no memorising)

| Mode | KEY1 (middle) | KEY2 (right) |
|------|---------------|--------------|
| READY | short → start recording | exit page (next page) |
| RECORDING | (short → stop early) | — |
| REVIEW | short → play · long → re-record | exit page |
| PLAYING | — | — |

Mnemonic: **KEY1 = do it (record / play; hold = re-record); KEY2 = leave.**

### Implementation notes (for later)

1. Add short/long-press detection (track key-down→key-up time; ≥1 s = long).
2. Make the REC page modal: when `app_page == PAGE_AUDIO_REC`, key events drive
   this state machine instead of page-nav; other pages unchanged.
3. Record/playback runs on the audio thread (UI only reads state) — HW-016e.

## 5. Acceptance criteria

- On hardware: enter REC, record a spoken phrase, play it back, and the phrase
  is **recognisable** (QR-1).
- Display stays stable; no I2S wedge across repeated record/play (QR-2, FR-6).
- Serial log shows state transitions + capture RMS; saved under `evidence/`
  (QR-5).
- `native_sim` ztest covers the new pure-logic DSP (mono↔stereo, gain/clip).
