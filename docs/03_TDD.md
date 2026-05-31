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

The custom drivers are unit-tested with I2C emulators on `native_sim`, so the
driver logic is covered without hardware (CI-friendly).

### M5PM1 MFD path (the on-board power path)

`tests/drivers/m5pm1_mfd` exercises the path the board actually uses: the
`m5stack,m5pm1` MFD parent (`mfd_m5pm1.c`) + its GPIO child (`gpio_m5pm1.c`) +
ADC child (`adc_m5pm1.c`) + a top-level `regulator-fixed` (the L3B/LCD rail)
switched through MFD GPIO2. (It replaces the earlier interim
`m5stack,m5pm1-l3b-regulator` ztest, deleted once this MFD path was
hardware-verified — HW-010.)

```bash
west build -p always -b native_sim -d build_test \
  tests/drivers/m5pm1_mfd -- -DZEPHYR_EXTRA_MODULES=$PWD
./build_test/zephyr/zephyr.exe
```

Result 2026-06-01: 4/4 pass. The devices init at boot in the on-board
POST_KERNEL order (I2C 50 < MFD 80 < adc 81 < gpio 82 < regulator-fixed 83); the
emulator seeds every driver-touched register to the OPPOSITE of its expected
post-init value and drops the first I2C transfer, so each assertion is
load-bearing. The 4 tests:

- **idle-sleep disable** — `mfd_m5pm1_init()` must write reg 0x09 = 0x00 (the
  local delta vs PR #109961); the seed is 0xFF, so a no-op fails.
- **wake-retry** — the emulator fails the first I2C transfer; the MFD must still
  come ready because init retries the ID read (remove the retry → not-ready).
- **PYG2 enable** — enabling the `regulator-fixed` drives PYG2 high via the full
  plain-GPIO push-pull sequence (FUNC0 cleared, MODE output, DRV push-pull, OUT
  high), auto-enabled by `regulator-boot-on`.
- **VBAT read** — `adc_read()` on logical channel 1 returns the seeded VBAT
  millivolts (a wrong register/byte-order/channel yields a different value).

### ES8311 codec

`tests/drivers/audio/es8311` exercises the in-repo ES8311 codec driver against
the Zephyr audio codec API with an I2C emulator:

```bash
west build -p always -b native_sim -d build_test \
  tests/drivers/audio/es8311 -- -DZEPHYR_EXTRA_MODULES=$PWD
./build_test/zephyr/zephyr.exe
```

Result 2026-06-01: 9/9 pass. Covers chip-ID read (and the wrong-ID
warn-and-continue path), the 16 kHz / 16-bit configure sequence + write
ordering, volume/mute set, unsupported-format and unsupported-property
rejection, and I2C-error propagation.

Honesty limits: each emulator is a dumb byte-store, NOT an independent oracle —
it cannot catch a register *meaning* error shared by driver+emulator (mitigated
by the on-hardware bring-up, which is independent: HW-003/010 for the LCD rail,
HW-006 for audio). So these are **logic-verified, not silicon-verified** — pair
with the hardware tests below.

### IR NEC encode/decode

`tests/drivers/ir_nec` unit-tests the protocol logic on `native_sim`, with no
hardware and no emulator (pure functions):

```bash
west build -p always -b native_sim -d build_test \
  tests/drivers/ir_nec -- -DZEPHYR_EXTRA_MODULES=$PWD
./build_test/zephyr/zephyr.exe
```

Covers `nec_encode()` (LSB-first addr/~addr/cmd/~cmd framing) and `nec_decode()`
fed synthetic mark/space duration vectors: an ideal frame, a jittered-but-in-
tolerance frame, a frame with a bad leader, one with a bad address/command
checksum, and a repeat frame. Decode must accept the valid ones and reject the
invalid ones. This is logic-verified; the carrier timing and the MCPWM capture
path are validated on hardware (HW-005).

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

Depends on the M5PM1 PMIC rail (L3B / PYG2) being enabled first. As of P3 the
rail is a top-level `regulator-fixed` (`lcd_power`) driven by the M5PM1 MFD gpio
child (`<&m5pm1_gpio 2>`), replacing the interim `m5stack,m5pm1-l3b-regulator`
node. Without the rail the panel is unpowered (dark).

Init-priority contract: the L3B `regulator-fixed` enables via the gpio child, so
it must init after GPIO_M5PM1 (82) and before the display (85). `prj.conf` sets
`CONFIG_REGULATOR_FIXED_INIT_PRIORITY=83` → order I2C(50) < MFD(80) <
ADC_M5PM1(81) < GPIO_M5PM1(82) < regulator-fixed(83) < display(85). The default
75 would init the rail before the gpio child (enable-gpio write fails → dark
panel); confirmed via `.config` and the linker priority `SORT`.

Pass criteria:

- LCD displays controlled content (color fill or status page).

Result (2026-05-30): PASS (interim L3B regulator). Panel cycles R/G/B/W fills;
serial `disp=ready`. Evidence: `evidence/20260530-lcd.log`.

Result (2026-06-01): PASS via the P3 MFD path. After the M5PM1 MFD restructure
(L3B as `regulator-fixed` on the gpio child) the LCD powers and shows the demo
dashboard - user-confirmed on hardware (read the POWER page). The init-priority
chain + the MFD idle-sleep/wake-retry handling work as designed. Boot/run log
`evidence/20260601-hw010-p3-battery-boot.log`.

### HW-004 BMI270

Pass criteria:

- Accel/gyro readings change when board is moved.

### HW-005 IR (NEC TX + RX)

IR is implemented on stock PWM drivers, not RMT (Zephyr 4.4 has no ESP32 RMT
driver): TX via the LEDC carrier on G46, RX via MCPWM input capture on G42, with
NEC encode/decode in `app/src/nec.c` (unit-tested, see "IR NEC encode/decode").
Gated `CONFIG_APP_IR`.

Pass criteria:

- TX: a transmitted NEC frame is observable - the IR LED flashes (visible through
  a phone camera) and/or a receiver decodes the known address/command.
- RX: pointing a known NEC remote at G42 decodes a stable, correct address and
  command on the serial log.
- Loopback (headline self-test): a frame transmitted on G46 is captured and
  decoded on G42 of the same device, matching what was sent.
- Speaker-amp interaction documented: the AW8737 amp is OFF during receive (it is
  off at rest; the rule is "do not beep while capturing").

### HW-006 audio

Pass criteria:

- ES8311 register access succeeds.
- I2S playback or loopback works at one sample rate.

Result (2026-06-01): PASS. The in-repo ES8311 codec driver (Zephyr audio codec
API, native_sim ztest 9/9) configures the codec at 16 kHz / 16-bit
(MCLK-from-BCLK) and a 440 Hz beep plays from the speaker on entering the AUDIO
page - user-confirmed. The AW8737 amp is enabled via the M5PM1 MFD gpio child
(masked write), and the LCD stayed lit when the amp toggled (L3B rail preserved).
The amp is off except during playback. Boot/run log
`evidence/20260601-hw006-p5-audio-boot.log`.

### HW-007 demo skeleton (modular app)

Verifies the P0 modularization introduced no regression and the page model works.

Pass criteria:

- Serial still prints the boot banner + periodic heartbeat (uptime/IMU/buttons).
- The LCD shows a stable frame (no crash or reset loop).
- Pressing KEY1/KEY2 changes the active page index, visible in the serial log.

Result (2026-05-31): PASS. Modular app boots clean (display ready 135x240,
healthy heartbeat, BMI270 gravity tracks Z); instant button navigation
user-confirmed on hardware (KEY1 next / KEY2 prev cycling HOME/IMU/DIAG,
semaphore-woken so presses respond immediately). Boot/run log
`evidence/20260531-hw007-p0-boot.log`. Button input-event log: see HW-002.

### HW-008 text rendering

Verifies the P1 RGB565 font blitter renders legible text on the ST7789V.

Pass criteria:

- PAGE_HOME shows readable, upright text (title + a live uptime line) on the LCD,
  with no garbling, mirroring, or rotation.
- The uptime value increments over time.

Result (2026-05-31): PASS. White "M5StickS3" + "up: N s" on black, legible and
upright, seconds incrementing - user-confirmed on hardware. gfx auto-detected
RGB_565 (high byte first). Boot/run log `evidence/20260531-hw008-p1-text-boot.log`.

### HW-009 live telemetry pages

Verifies P2 live data + button page navigation.

Pass criteria:

- Four pages (HOME/IMU/POWER/DIAG) cycle with KEY1 (next) / KEY2 (prev); each
  info page shows a page-name header.
- The IMU page shows live accel X/Y/Z that track board orientation, with correct
  signs (a downward axis reads negative).
- Info-page refresh does not flash the whole panel (clear only on page change).

Result (2026-05-31): PASS - user-confirmed on hardware (all four pages present and
cycling, live IMU values with correct signs, no flicker). Boot/run log
`evidence/20260531-hw009-p2-pages-boot.log`.

### HW-010 battery readout (M5PM1 ADC, P3)

Verifies the P3 M5PM1 ADC integration: `status_sample()` reads VBAT via the
Zephyr ADC API on `m5pm1_adc` channel 1 (gain ADC_GAIN_1, ref ADC_REF_INTERNAL,
acq DEFAULT, 12-bit). The M5PM1 VBAT register already reports millivolts, so the
sample is used directly (no `adc_raw_to_millivolts`). PAGE_POWER and the HOME
`bat:` line show "VBAT/bat: %d mV" (or "n/a" on read failure / -1).

Pass criteria:

- PAGE_POWER shows a plausible VBAT value in mV (roughly 3000-4200 mV on
  battery) that tracks charge/load; HOME `bat:` shows the same.
- A read failure degrades to "n/a", not a crash.

Result (2026-06-01): PASS - runtime-verified on hardware. PAGE_POWER showed
`VBAT: 4182 mV` (matches the issue #4 ADC validation of ~4180 mV); HOME `bat:`
shows the same. The MFD-based L3B rail powered the LCD correctly (no dark panel),
confirming the idle-sleep + init-priority handling work on real silicon. Boot/run
log `evidence/20260601-hw010-p3-battery-boot.log`.

### HW-011 BLE advertising (manufacturer telemetry)

Verifies P4 connectable advertising with live manufacturer-data telemetry.

Pass criteria:

- A scanner sees "StickS3" advertising with manufacturer data (company 0xFFFF)
  whose decoded payload (uptime / battery mV / accel mg) is sensible and updates.

Result (2026-06-01): PASS - verified from the PC via bleak (Windows BLE): 26
adverts in 8 s, decoded uptime increments, battery ~4176-4188 mV (matches
HW-010), accel Z ~ +1020 mg (gravity). Evidence:
`evidence/20260601-hw011-012-p4-ble.md`.

### HW-012 BLE GATT (read + notify) and advertising restart

Verifies the connectable GATT telemetry service and that advertising resumes
after a client disconnects.

Pass criteria:

- A client connects, reads the telemetry characteristic, and receives ~1 Hz
  notifications with live values.
- Advertising resumes after disconnect (the device stays discoverable).

Result (2026-06-01): PASS - PC bleak connected, READ returned valid telemetry, 6
notifications at ~1 Hz (live uptime). A hardware-only bug was found: the device
stopped advertising after the first connect/disconnect (no restart). Fixed by
deferring bt_le_adv_start to a k_work (inline restart in the disconnect callback
returned -EAGAIN); after the fix a re-scan saw 28 adverts in 8 s following a
disconnect. Evidence: `evidence/20260601-hw011-012-p4-ble.md`.

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
