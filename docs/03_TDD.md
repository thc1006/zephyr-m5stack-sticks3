# Test-Driven Development Plan

## Testing philosophy

Embedded board-port TDD is not only unit testing. It combines:

1. Host-side structure tests.
2. Build-only tests through Zephyr/Twister.
3. Hardware-in-the-loop/manual evidence tests.
4. Regression logs for each board milestone.

## Test pyramid

```text
Hardware evidence tests       few, high value, manual/HIL
Twister build tests           medium quantity, automated when Zephyr exists
Host structure tests          many, fast, always runnable
Markdown/source checks        always runnable
```

## Always-runnable host tests

Run:

```bash
bash verify.sh
```

Covers:

- required files exist.
- forbidden AI co-author footers absent.
- source pin table file exists.
- shell scripts pass `bash -n`.
- markdown files have no obvious unresolved placeholders except intentional TODO markers.

## Zephyr build tests

Run after bootstrapping Zephyr:

```bash
bash scripts/build_m5sticks3.sh
```

Target acceptance:

- Board target is discoverable.
- Validation app compiles.
- Build artifacts generated under `build/`.

## Driver unit tests (emulator, no hardware)

The M5PM1 PMIC driver is unit-tested with an I2C emulator on `native_sim`, so the
driver logic is covered without hardware (CI-friendly):

```bash
west build -p always -b native_sim -d build_test \
  tests/drivers/regulator/m5pm1 -- -DZEPHYR_EXTRA_MODULES=$PWD
./build_test/zephyr/zephyr.exe
```

Result 2026-05-30: 6/6 pass (`evidence/20260530-ztest-m5pm1.log`). The suite was
adversarially reviewed and hardened so each assertion is load-bearing (the
emulator seeds every driver-touched register to the opposite of the expected
state, exploiting that `i2c_reg_update_byte` skips no-op writes). Covers:

- boot-on enable writes the correct PYG2 registers (FUNC0/MODE/DRV/OUT) + clears idle-sleep;
- `disable`/`enable` round-trip and refcount;
- enable() re-asserts the FULL sequence after the config is corrupted (idempotent);
- write ordering: push-pull (DRV) before drive-high (OUT), OUT written last;
- neighbour GPIO (PYG3) bits preserved across the per-pin RMW;
- I2C transfer failure propagates as an error (fault injection).

Honesty limits (from the adversarial review): the emulator is a dumb byte-store,
NOT an independent oracle — it cannot catch a register *meaning* error shared by
driver+emulator (mitigated by the on-hardware LCD bring-up, which is independent).
Not yet covered: a DEVICE_ID-mismatch test (needs a second emulator instance) and
an on-hardware I2C-trace parity check vs the M5GFX init sequence. So: this is
**logic-verified, not silicon-verified** — pair with the hardware tests below.

## Hardware tests

### HW-001 boot + console

Steps:

1. Put StickS3 in download mode.
2. Flash validation app.
3. Open serial monitor.
4. Capture boot banner.

Pass criteria:

- Serial console prints `M5StickS3 Zephyr validation app`.
- No reset loop for 60 seconds.

### HW-002 buttons

Pass criteria:

- KEY1 and KEY2 press events are visible in serial log or display state.

Note (2026-05-30): verified at the GPIO level (G11/G12 read low on press,
`evidence/20260530-buttons.log`) AND via the gpio-keys interrupt/input-event
path — pressing KEY2 produced `BUTTON code=2 PRESSED`/`released`
(`evidence/20260530-b34.log`). The earlier zero-event captures were mistimed
presses, not a broken path.

### HW-003 LCD

Depends on the M5PM1 PMIC rail (L3B / PYG2) being enabled first — see the
`m5stack,m5pm1-l3b-regulator` driver. Without it the panel is unpowered (dark).

Pass criteria:

- LCD displays controlled content (color fill or status page).

Result (2026-05-30): PASS. Panel cycles R/G/B/W full-screen fills; serial shows
`disp=ready`. Evidence: `evidence/20260530-lcd.log`. Photo/video still to add.

### HW-004 BMI270

Pass criteria:

- Accel/gyro readings change when board is moved.

### HW-005 IR

Pass criteria:

- IR TX produces observable waveform or receiver confirms known protocol.
- Speaker amplifier interaction is documented.

### HW-006 audio

Pass criteria:

- ES8311 register access succeeds.
- I2S playback or loopback works at one sample rate.

## Evidence filenames

```text
evidence/YYYYMMDD-HHMM-build.log
evidence/YYYYMMDD-HHMM-flash.log
evidence/YYYYMMDD-HHMM-serial.log
evidence/YYYYMMDD-HHMM-photo.jpg
evidence/YYYYMMDD-HHMM-video.mp4
```

## Results snapshot (2026-05-30)

Runtime-verified on a physical StickS3 (serial logs in `evidence/`):

- HW-001 boot/console: PASS.
- HW-002 buttons: PASS (GPIO level + gpio-keys input events, `BUTTON code=2`).
- B-3 regulator disable: PASS on HW (backlight blinks off/on via regulator API).
- HW-003 LCD: PASS (color fill; needs M5PM1 L3B rail driver).
- HW-004 BMI270: PASS (gravity tracks across axes).

Evidence (per `CONTRIBUTING.md`): build/flash/serial logs under `evidence/`, plus
LCD photos `evidence/PXL_20260529_2351*.jpg` (panel lit, cycling colours). Audio/
IR/full power-management remain roadmap and must not be claimed until they have
their own captured evidence.

The new M5PM1 regulator lives in an out-of-tree module (`drivers/`,
`dts/bindings/`, `zephyr/module.yml`); `scripts/build_m5sticks3.sh` adds it via
`ZEPHYR_EXTRA_MODULES`, so a normal `west build` compiles it.
