# IR (NEC) hardware session - 2026-06-01

Physical M5Stack StickS3 (ESP32-S3-PICO-1-N8R8), Zephyr 4.4.0, `overlay-ir`
build (`CONFIG_APP_IR`). IR on stock peripherals (Zephyr 4.4 has no ESP32 RMT
driver): TX via the LEDC ~38 kHz carrier on G46; RX via a GPIO edge interrupt on
G42 (MCPWM input capture was tried first but drops edges during the fast 67 ms
NEC burst).

## Result: TX and RX both runtime-verified

- **IR TX (NEC): PASS.** The transmitter emits NEC frames on G46; serial shows
  repeated `ir_tx addr=0x04 cmd=0x1b (#n)`, also visible as an IR-LED flash on a
  phone camera. The IR init path is clean and the LCD stays lit (no DT
  regression).
- **IR RX (NEC): PASS.** The G42 receiver works and the NEC decode path works:
  - Receiver: a real TV remote produced ~10k edges in 40 s on the G42 line (HOME
    page, TX silent) - it receives external IR of any protocol.
  - NEC decode: an on-device TX->RX loopback decodes the transmitted frame
    (`IR RX addr=0x04 cmd=0x1b`); the GPIO RX measures a real 9 ms + 4.5 ms NEC
    leader (the earlier "147 us leader space" was an MCPWM edge-drop artifact).
- **Scope:** the tested remote is not NEC, so it is not decoded to addr/cmd, but
  it still registers on the "IR act" edge counter on the IR page. Decoding other
  protocols (RC5/RC6/SIRC/...) is out of scope: Zephyr has no IR subsystem to
  host protocol decoders. NEC is the supported reference protocol.

## Path notes / what was learned

- The Zephyr ESP32 **MCPWM continuous-capture** driver drops edges during the
  NEC burst (only ~5-6 of 32 bits captured per frame, plus a distorted leader),
  so it is unsuitable for IR RX. A **GPIO edge interrupt** + cycle-counter
  timestamp captures every edge reliably (NEC edge rate ~1 kHz is trivial).
- Only a real 9 ms mark + 4.5 ms space starts a frame, so mains-light flicker
  (which appears as ~9 ms marks with a tiny space) is rejected and idle is clean.
- RX is suspended during a TX burst: the device must not receive its own carrier,
  and the RX ISR would otherwise jitter the software-timed TX. The IR page
  transmits one frame on entry, then stays quiet so RX can receive cleanly.

## Evidence files

- `20260601-ir-init-boot.log` - boot clean, `ir_init OK`, `IR RX on GPIO edge
  interrupt (G42)`, LCD ready, no reset loop.
- `20260601-ir-tx.log` - TX emitting (`ir_tx addr=0x04 cmd=0x1b`).
- `20260601-ir-rx-edges.log` - a real remote producing ~10k edges on G42 (HOME
  page, TX silent): the receiver works, even though that remote is not NEC.
- `20260601-ir-rx-ambient.log` / `20260601-ir-idle-clean.log` - the MCPWM
  edge-drop diagnostic and the post-fix clean idle.

## Reproduce

```bash
bash scripts/build_demo.sh   # or: west build ... -- -DEXTRA_CONF_FILE=overlay-ir.conf
# flash zephyr.bin at 0x0 with esptool --after watchdog-reset (COM port)
# IR page: one TX on entry (phone camera shows the LED). Point any remote at the
# top: "IR act" climbs; a NEC remote also shows "NEC AA:CC".
```
