# ES8311 clock and oversampling: the primary sources

Written 2026-08-09. These are facts with citations, gathered so they do not have to be
re-derived. Two of them contradict things this repository previously asserted.

## DAC_OSR is a multiple of fs, not a divider

Datasheet **revision 8.0, May 2020, section 8** is the newest public register-level
document. It defines:

- `0x03` as `ADC_FSMODE` (bit 6) plus `ADC_OSR` (bits 5:0)
- `0x04` as `DAC_OSR` (bits **6:0**)

Both encode a field value N as an oversampling rate of **4N x fs**: 16 is 64 x fs, 32 is
128 x fs, 64 is 256 x fs. Reset default 0x10 for both.

**Consequence, and it corrects this driver's own earlier comment.** The clock tree here is
ratiometric, BCLK x 8 on a 16-bit frame giving 256 x Fs, and that argument genuinely covers
0x02, 0x05, 0x06, 0x07 and 0x08, which are divisions of the master clock. It does **not**
cover 0x03 or 0x04, which are multiples of the sample rate. The driver used to say it did.

Bit 7 of both registers is undocumented. `esp_codec_dev` preserves it with a read-modify-write
masking 0x80; this driver writes the whole byte, which clears it. That is deliberate: this
driver never resets the part, so its contract is to write every register it depends on into a
known state rather than inherit one, and preserving that bit would preserve exactly what the
contract exists to discard. The reset default is 0, so a cold part is identical either way.

## The low-rate DAC_OSR change is a named vendor bug fix

**esp-adf commit `13c3bcff653aef6af1b11e9ba3a9bd68bf21355b`, 2026-03-08**, titled
*"audio_hal: fix the es8311 noise on 8k-16k"*. Machine-compared rather than eyeballed: it
changes **exactly one column**, `dac_osr`, on **exactly the rows for 8000, 11025, 12000 and
16000 Hz**, from `0x10` to `0x20`. No divider, no BCLK, no LRCK, no `adc_osr`, no `fs_mode`.

That driver pins `MCLK_DIV_FRE 256`, so the noise was observed **in this clock tree, at these
four rates**, which are this driver's four lowest.

`adc_osr` is `0x10` in every single-speed row of the reference table, the sole exception being
a 64 kHz row this driver does not support. So ADC_OSR really is rate-independent and DAC_OSR
really is not.

Who carries the fix: `esp_codec_dev` and ESPHome do. **esp-bsp does not, and it is not an
independent second opinion**: its copy was added 2025-12-03, before the 2026-03-08 fix, and
every commit to it since is unrelated or formatting. Mainline Linux deliberately goes the other
way and never writes 0x03 or 0x04 at all, keeping the reset defaults at every rate; that is the
one real counter-precedent, and it targets the part generically.

## HW-030 measured the wrong quantity

Two images differing only in that define were swept over seven rates on one board. Both passed
at every rate, the measured LRCK was identical to the sample in both, and the part accepted and
read back 0x20. Evidence: `evidence/20260809-hw030-es8311-dac-osr-ab-0x10-serial.log` and
`...-0x20-serial.log`.

**The run cannot settle the question and never could.** The vendor change is about *noise*; the
sweep measured the RMS of a tone and the ADC noise floor with the amplifier off. Neither is DAC
output noise. So the null result is not weak evidence for the old value, it is no evidence
either way.

The rate sweep has since gained a `dacfloor` phase, amplifier on with digital silence on the
wire, which is the only configuration in it that can see the quantity in question. **That phase
has not been run on hardware yet.**

## Revision 17.0 Note 2: a clock floor that did not exist in Rev 8.0

**Everest ES8311 Product Brief, Revision 17.0, February 2026**, section 7, note 2:

> The internal clock source can be MCLK or SCLK. When this internal clock source is multiplied
> by 4 or 8, its frequency must be greater than 1 MHz for 3.3V DVDD or 500 kHz for 1.8V DVDD.

**This clause does not exist in Rev 8.0 (May 2020) or Rev 7.0 (January 2020).** It is only at
`http://www.everest-semi.com/pdf/ES8311 PB.pdf`, which is http-only, and that document is a
product brief with no register map. The datasheet M5Stack itself links is Rev 5.0; Espressif's
copy is Rev 7.0. **Neither shows this note**, so a reviewer reading the vendor-hosted PDFs will
not find it.

**DVDD on this board is 3.3V, confirmed from the schematic** (`K150_Stick_S3_PRJ_V0.6_20251111`,
linked from docs.m5stack.com): ES8311 is `U18`, and PVDD (pin 3), DVDD (pin 4) and AVDD (pin 11)
are all on one net `3V3_L3B_AU`, the output of `U1 = CM1801F33` gated by `PYG2_L3B_EN`. The
board has **no 1.8V rail at all**. So the 1 MHz threshold applies, not 500 kHz.

This driver uses MULT_PRE x8 at every rate with 16-bit words, so the multiplier input is
BCLK = 32 x Fs:

| Fs | multiplier input | > 500 kHz | > 1 MHz |
|---|---|---|---|
| 8000 | 256 kHz | no | no |
| 11025 | 352.8 kHz | no | no |
| 12000 | 384 kHz | no | no |
| 16000 | 512 kHz | yes | no |
| 22050 | 705.6 kHz | yes | no |
| 24000 | 768 kHz | yes | no |
| 32000 | 1.024 MHz | yes | yes |
| 44100 | 1.4112 MHz | yes | yes |
| 48000 | 1.536 MHz | yes | yes |

**Do not assert either reading of the note.** "Its frequency" is genuinely ambiguous: the
pre-multiplication source, which gives the table above, or the resulting internal master clock
at 256 x Fs, under which every rate passes. Against the strict reading: HW-019 is 9 of 9 rates
passing with LRCK within 5 Hz and a noise floor that *rises* with rate rather than degrading at
the low end.

**This corrects the earlier claim that "the 512 kHz BCLK floor is not real".** The measurement
stands; the reasoning does not. The floor is a datasheet clause, stated differently, and this
board runs outside it and happens to work.

## A second Espressif 8 kHz fix this driver does not have

esp-adf issue 1506 and commit `596cd81bae`, 2025-08-20, fixes crackling at 8 kHz **when MCLK is
derived from BCLK**, which is exactly this driver's configuration, by forcing 32-bit slots so
BCLK becomes 64 x Fs. This driver rejects anything but 16-bit words, so that route is closed to
it as written.

## Revision 17.0 Note 5, against the no-reset design

> recommend to provide MCLK, LRCK and SCLK before control register setting, otherwise reset
> after MCLK, LRCK and SCLK are provided.

This driver configures over I2C with no bit clock running and deliberately never resets, which
is the "otherwise" branch the note says should be followed by a reset. See
`docs/15_ES8311_NO_RESET_DESIGN.md` for why the reset is refused, and treat this as an open
tension rather than a settled one.
