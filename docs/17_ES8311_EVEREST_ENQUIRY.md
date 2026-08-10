# Enquiry to Everest Semiconductor about the ES8311

Three questions we cannot settle from the public documents, each of which changes what an
upstream driver is allowed to claim. Written 2026-08-10, to be sent to `info@everest-semi.com`
(the address given in the Product Brief).

Two of them are already being handled the conservative way in
[`drivers/audio/es8311.c`](../drivers/audio/es8311.c), so a reply is not blocking; it would let
us widen what the driver supports rather than narrow it. See
[`docs/16_ES8311_CLOCK_AND_OSR_EVIDENCE.md`](16_ES8311_CLOCK_AND_OSR_EVIDENCE.md) and
[`docs/15_ES8311_NO_RESET_DESIGN.md`](15_ES8311_NO_RESET_DESIGN.md) for what we did establish.

---

## The letter

**Subject:** ES8311 clarifications: REG02 multiplier input limit, REGFA INI_REG behaviour, and DAC_OSR at low rates

Hello,

I maintain an open source ES8311 driver for the Zephyr RTOS. The work is public, and I would
rather have your answer in it than my own inference, so I am writing before we finalise what the
driver advertises.

The board is an M5Stack StickS3. The codec runs as an I2S target, and its supplies are confirmed
from the schematic: PVDD, DVDD and AVDD share one 3.3 V rail, and the board has no 1.8 V rail at
all.

### 1. The multiplier input limit in REG02

Revision 17.0 of the Product Brief, section 7, note 2, says that the internal clock source can be
MCLK or SCLK, and that when this source is multiplied by 4 or 8 its frequency must be greater
than 1 MHz for 3.3 V DVDD, or 500 kHz for 1.8 V DVDD.

Our configuration is:

```text
MCLK_SEL = SCLK
DIV_PRE  = /1
MULT_PRE = x8
SCLK     = 32 * Fs      (16-bit words, two slots)
DVDD     = 3.3 V
```

The sentence is ambiguous to us in one specific way. Does "its frequency" refer to the input of
the multiplier, which here is SCLK divided by the pre-divider, or to the resulting internal
master clock after multiplication?

Read as the input, only three of the rates we would like to support qualify:

| Fs (Hz) | multiplier input (Hz) | above 1 MHz |
| ---: | ---: | :--- |
| 8000 | 256000 | no |
| 11025 | 352800 | no |
| 12000 | 384000 | no |
| 16000 | 512000 | no |
| 22050 | 705600 | no |
| 24000 | 768000 | no |
| 32000 | 1024000 | yes |
| 44100 | 1411200 | yes |
| 48000 | 1536000 | yes |

Read as the output, every rate passes, since the result is 256 * Fs in all cases.

Three things would help:

- Which of the two readings is intended?
- Is the limit strictly greater than 1 MHz, or is exactly 1 MHz acceptable?
- For 8 kHz through 48 kHz, is an external MCLK of 256 * Fs with `DIV_PRE = /1` and
  `MULT_PRE = x1` the configuration you would recommend, since it uses no multiplier?

We have taken the input reading, because it is the one that constrains rather than excuses, and
the driver currently accepts the BCLK-derived mode only at 32, 44.1 and 48 kHz. Nine rates do
work on our hardware in that mode, but a bench result on one board is not a guarantee across
process, voltage and temperature, so we did not treat it as one.

### 2. REGFA bit 0, INI_REG

The register description we have says only that INI_REG resets the registers to their defaults
except itself. We could not find anything further, and we would rather not model behaviour we
cannot cite. Could you confirm:

- Is the bit a pulse or a level?
- Is it self-clearing, or does it stay set until written back to zero?
- What value does REGFA itself read back while the bit is asserted?
- While it is asserted, do reads of other registers return their default values, return zero, or
  behave normally?
- Are writes to other registers ignored while it is asserted, or do they take effect?
- What is the required sequence for releasing it, including any settling time?
- Does the asserted state survive a host reset while codec power stays up?

The reason for asking is concrete. An ES8311 driver shipped in some vendor kernels asserts this
bit in its shutdown handler and never clears it, so a part can plausibly be handed to the next
firmware in that state. Our driver clears it once during initialisation, after reading the chip
identity, but we would like to know whether that is sufficient and correctly ordered.

### 3. DAC_OSR below 22.05 kHz

Reference code we have seen uses `0x04 = 0x20`, that is DAC_OSR at `128 * fs`, for 8000, 11025,
12000 and 16000 Hz, and `0x10`, at `64 * fs`, at 22.05 kHz and above, with a change described as
fixing audible noise in the 8 kHz to 16 kHz range. Is that split the recommended one, and does it
depend on how the internal master clock is produced? Our driver follows it, but we would like to
know whether it addresses a known effect or is specific to one clock arrangement.

Finally, could you point us at the most recent public datasheet or user guide revision that
defines these behaviours? The copies we can reach are revision 5.0, 7.0, 8.0 and the 17.0 Product
Brief, and the register-level content differs between them.

Thank you for your time.

Hsiu-Chi Tsai
