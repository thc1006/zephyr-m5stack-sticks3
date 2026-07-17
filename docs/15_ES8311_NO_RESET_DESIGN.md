# ES8311: why the driver does not reset the codec, and what that costs

This is the design record behind `drivers/audio/es8311.c` and upstream Zephyr PR
[#113312](https://github.com/zephyrproject-rtos/zephyr/pull/113312). It exists so the
upstream pull request can be a specification rather than a diary, without the evidence
being lost.

It also records **two hypotheses that were wrong**, and the measurements that killed them,
because both of them were committed to the driver and to a public PR before they were
refuted. Anyone re-deriving this from scratch would very likely make the same two mistakes,
and the sweep that produced the first one is still in the tree.

---

## The decision

**The driver never asserts a reset.** It reaches a known register state the long way, by
writing every register whose value its behaviour depends on.

Three reasons. Only the third one is about cost; the first two are about the reset not
existing in the first place.

### 1. There is no reset pin

The ES8311 has twenty pins: the I2C three, `MCLK`, `SCLK`, `LRCK`, `ASDOUT`, `DSDIN`,
`MIC1P`/`MIC1N`, `OUTP`/`OUTN`, four supply pairs, and three analog filtering-capacitor pins
(`VMID`, `ADCVREF`, `DACVREF`). There is no signal to wire a reset line to.

An earlier revision of the binding had a `reset-gpios` property and the driver asserted it.
Both are gone. **Do not add them back.**

### 2. Register `0x00` does not reset the register file

`RST_DIG` is documented as *"reset digital except control port block"* -- and the control
port block is where the registers live. The vendor sequence (`0x00 = 0x1F`, wait,
`0x00 = 0x00`, `0x00 = 0x80`) resets the digital blocks and leaves **every register exactly
as it found it**.

The only register-file reset on the part is `0xFA` bit 0, and that one is unusable as a
reset (see below).

### 3. What a reset actually costs: ~6.2 seconds of a stone-deaf ADC

Measured, HW-023 (`evidence/20260712-hw023-es8311-reset-is-innocent-adc-needs-6s.log`):

> after `0x00 = 0x1F`, the capture noise floor is **exactly zero for about 6.2 seconds**,
> then normal, then normal forever.

Not a rate cutoff. Not a filter. Not a bandwidth limit. A flat ~6.2 s of nothing, **at every
sample rate**, and then it works.

Reaching the same known state by writing the registers instead costs nothing measurable: on
a settled part the capture noise floor was **103 counts before those writes and 109 after**,
with no settling transient.

---

## Two hypotheses that were wrong

### Wrong #1: "the software reset deafens the ADC below ~22 kHz"

This was in the driver, in the devicetree binding, and in the upstream PR body. **It was
false.**

The rate sweep that produced it walks 8 kHz up to 48 kHz, at roughly two seconds per rate.
So it increases the sample rate **and** the time since reset **at the same time**. "Deaf
below 22 kHz" and "deaf for the first ten seconds" fit the same nine numbers, and **nothing
in any run of that sweep can tell them apart.** Rate order and time order are collinear.

Reversing the rate table separates them, and there is no third outcome:

| measured | order | result |
|---|---|---|
| 48 kHz | **first** | came back **DEAF** |
| 8 kHz | **last** | came back **ALIVE** |

There is no low-rate cutoff and there never was. **`CONFIG_APP_AUDIO_RATE_SWEEP` still
ascends.** Its header comment, its baseline comment and the banner it prints into every
evidence log now all say so. Do not read a low-rate cliff off that sweep without reversing
the table first.

### Wrong #2: "the six seconds are the reference capacitors recharging, so a cold boot pays them too"

Also false, and **it was already committed** to the driver, the binding and the risk
register when a physical unplug killed it.

A genuine cold power-on charges those same capacitors from zero. The ADC is **alive at the
earliest moment it can be sampled**, 2.2 s after power is applied. At that same 2.2 s mark
after a register reset, the floor is zero.

Three cases, same elapsed time (HW-026,
`evidence/20260713-hw026-es8311-cold-boot-is-not-deaf.log`):

| at t ≈ 2–3 s | noise floor | |
|---|---|---|
| cold power-on | 10053, decaying to baseline | **ALIVE** |
| warm boot | 93, flat | **ALIVE** |
| after `0x00` reset | 0 | **DEAF** |

So the reset is **not** "the cold start you were going to pay anyway, brought forward". It
puts the part somewhere a cold start never goes.

A follow-up tried to force the VMID reference to charge faster to see if that shortened the
window. **HW-025 was inconclusive**
(`evidence/20260713-hw025-es8311-vmid-fast-charge-inconclusive.log`).

### What remains unknown

**Why the reset costs ~6.2 seconds is not known.** The measurement stands; the explanation
does not. No datasheet and no reference driver specifies any settling time -- Linux's own
`es8311.c` says so in as many words (*"Specific delay is not documented"*) -- so there is
nothing to look up.

It is recorded as *measured and unexplained*, and the driver simply never causes it.

---

## The defect class the no-reset design creates

A codec that is never reset **inherits its whole register file** from whatever ran last: a
vendor bootloader, another OS, an earlier firmware, or its own previous boot.

The driver's own comments used to claim *"nothing below may rely on reset defaults"* while
**eleven registers went unwritten**. The comment was aspiration, not description. The driver
now writes a known state for all eleven (`0x0B 0x0C 0x0F 0x10 0x11 0x18 0x19 0x1A 0x33 0x34
0x35`).

Five of them are not housekeeping. They are correctness:

| register | what a stale value does |
|---|---|
| **`0x18` `ALC_EN`** | The datasheet, under the ADC volume register itself: *"When ALC is on, `ADC_VOLUME` = MAXGAIN"*. A stuck ALC bit silently turns `0x17` -- which the driver writes and exposes as `AUDIO_PROPERTY_INPUT_VOLUME` -- from a **volume** into a **servo-loop ceiling**. |
| **`0x34` `DRC_EN`** | The exact mirror image, for the DAC volume (`0x32`). |
| **`0x44` bit 7 `ADC2DAC_SEL`** | Routes the ADC into the DAC. It is a **playback** control living in a register whose other field is about the ADC -- so it was only ever written on the capture path. A chip handed over with bit 7 set **plays its own microphone instead of the caller's audio**, through a route that just powered that microphone down. Silently: `configure()` returns 0 and every register the driver checks reads back as intended. |
| **`0xFA` bit 0 `INI_REG`** | **A LEVEL, not a pulse.** While it is set, the register file is *held* at its defaults: every write is silently discarded, every read returns `0x00`, and `configure()` still returns 0. The vendor Linux driver **sets this bit at shutdown on purpose**, to hold the file down across a reboot -- so a driver that never clears it can be handed a chip it cannot possibly recover. It is now the first register write the driver makes -- **and an adversarial review proved that is not enough.** `init()` read the chip id BEFORE writing anything, and a held part answers `0x00` to that read, so the identity check failed, `init()` returned `-ENODEV`, and the one write that recovers the chip was never reached. The driver documented the hazard exactly and then gated the cure behind a check the hazard defeats. **`es8311_check_id()` now treats a chip id of `0x0000` as a symptom rather than an identity**: it releases `0xFA` and re-reads, and anything that still fails to identify itself is rejected exactly as before. |
| **`es8311_init()` wrote zero registers** | With no reset pin, a warm reboot does not reach the codec, but the SoC's I2S peripheral *does* reset and the bit clock stops -- and this codec takes its master clock from that bit clock. A DAC that was powered and unmuted when the reboot hit has its modulator **frozen on its last sample**: a DC level sitting on the amplifier until the application gets around to `configure()`. `init()` now releases the register file and quiesces the part. |

`0xFA` was found the hard way: by setting it and forgetting to clear it. The next boot came
up with the codec stone deaf, the frame clock measuring a perfect 7999 Hz, and
`audio_init()` reporting success.

---

## Why the tests could not see any of this

All 29 of the original ztests started from an **all-zero emulator register file** -- a chip in
exactly the state the driver would like to find it in. **The suite structurally could not
observe the one defect class its own design premise creates.**

There are **42 now**. Several seed a **dirty** register file first, and they fail on the driver
as it stood (verified by stashing the fix, not by reasoning about it).

One of them is a **tripwire**: it scans the emulator's write log and fails if the driver ever
asserts `0x00[4:0]`. Deviating from every reference implementation is exactly the kind of thing
a future contributor helpfully undoes, and the cost of that is invisible to any test that is
not specifically looking for it. It checks what the driver **wrote**, not a wiped register
file -- because, per section 2 above, the silicon does not wipe one.

---

## The test that could not see the bug it was named after

`test_ini_reg_is_released_before_anything_else` existed, passed, and proved nothing about
the thing it is named after.

**The emulator did not model INI_REG.** It was a plain 256-byte register store, so a "held"
part could not exist in it. The test could only assert that the first entry in the write log
was `0xFA` -- which was true, and which did not stop `init()` from bailing out on the chip-id
read before ever getting there.

**A test cannot catch a bug in a state its model cannot express.** The emulator now models
`0xFA` bit 0 as a level: while set, every write to any other register is discarded and every
read returns `0x00`, and the only way out is a write to `0xFA` itself.

The same review found that the emulator **wiped its whole register array** when the low five
bits of `0x00` were asserted -- implementing a register-file reset that
[this very document](#2-register-0x00-does-not-reset-the-register-file) says the silicon does
not perform. The tripwire test depended on that fiction. It now checks the **write log**
instead: what the driver wrote, not what an imaginary chip did in response.

---

## The two fail-open bugs a later review found, and what they have in common

Both were **the driver being safe in what it says and unsafe in what it does**, and neither was
visible in the register values a test reads at the end of a `configure()`.

**The unmute came too early.** `configure()` wrote the final, un-muted serial ports right after
the clock tree, and un-muted the DAC in the middle of the playback branch -- with fifteen writes
still to come, one of which is `0x44`. And `0x44` bit 7 is `ADC2DAC_SEL`, the bit that decides
whether the speaker is playing the caller's audio or its own microphone. **So the fix for the
stale-`0x44` hazard spent its own window re-creating the hazard**, with a live speaker on a
stale mux. The last three writes of a `configure()` are now the two serial ports and the DAC
mute, in that order, and nothing that can fail happens after them. Only the write *order* can
show this; the end state cannot.

**A failed `configure()` forgot a converter instead of stopping one.** It clears its route cache
before touching the chip, which is right -- a half-reprogrammed part is not described by the old
route. But on its own that is a fail-open. Take a live capture route whose reconfigure fails on
its first write: the ADC is still powered, the PGA is live, MIC1 is still on the mux -- and the
cache now says there is no capture, so `apply_properties()` will not touch the ADC and
`stop_output()` only reaches the DAC. **The `audio_codec` API has no `stop_input()`. The
microphone was left running with no call in the API able to switch it off.** `init()` and every
error path out of `configure()` now run the same best-effort quiesce, and a test walks the
failure across every transfer and reads the registers back *before* calling anything else.

---

## 8 kHz stays in `es8311_rates[]`

Both Espressif reference drivers special-case 8 kHz (x4 instead of x8), on the claim that
BCLK must be at least 512 kHz.

- The datasheet does not state that anywhere.
- Linux's `es8311.c` contradicts it (identical `REG02 = 0x18` / `REG05 = 0x00` at every rate).
- **Espressif's own 11.025 kHz row contradicts it** -- x8 at 352.8 kHz, which is below their
  own claimed floor.

Measured on this silicon (HW-019/HW-020): **8 kHz works.** The frame clock lands at 7999 Hz
against the kernel cycle counter, and the ADC is alive. The special case is not needed and
`es8311_rates[]` keeps `8000U`.

---

## Evidence index

| | |
|---|---|
| `evidence/20260712-hw023-es8311-reset-is-innocent-adc-needs-6s.log` | the ~6.2 s deaf window, at every rate |
| `evidence/20260713-hw026-es8311-cold-boot-is-not-deaf.log` | a cold boot is ALIVE at t=2.2 s → hypothesis #2 refuted |
| `evidence/20260713-hw025-es8311-vmid-fast-charge-inconclusive.log` | VMID fast-charge: inconclusive |
| `evidence/20260712-hw019-reset-then-a-deaf-adc-SUPERSEDED.log` | the original sweep. **Its filename used to assert the false claim.** |

See also [`docs/06_RISK_REGISTER.md`](06_RISK_REGISTER.md) -- *"A test that measures the codec
while it is still settling will read the settling as a frequency response"*.
