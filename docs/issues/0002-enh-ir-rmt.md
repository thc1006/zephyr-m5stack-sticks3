<!--
Suggested GitHub labels: Enhancement, platform: ESP32
gh issue create --title "M5StickS3: IR TX/RX via ESP32 RMT" \
  --body-file docs/issues/0002-enh-ir-rmt.md --label "Enhancement"
-->
# Enhancement: M5StickS3 IR TX/RX via ESP32 RMT

**Is your enhancement proposal related to a problem? Please describe.**
The M5Stack StickS3 has an IR transmitter (G46) and receiver (G42), but the
current board port does not enable them. There is no IR/consumer-IR support
wired up for the board.

**Describe the solution you'd like**
Bring up IR using the ESP32 RMT peripheral:
- Confirm RMT (TX/RX) support state in the ESP32-S3 HAL/Zephyr tree.
- Add the IR TX (G46) / RX (G42) pins via pinctrl + an RMT-based node.
- Provide a sample that transmits a known protocol (e.g. NEC) and/or decodes RX.
- Document the hardware caveat: the StickS3 speaker amplifier (M5PM1 PYG3) must
  be disabled before IR receive (vendor warning).

**Describe alternatives you've considered**
Bit-banging IR on GPIO (rejected: timing accuracy/CPU load; RMT is the correct
ESP32 path).

**Additional context**
- Pins from the vendor PinMap: IR_TX G46, IR_RX G42 (see `docs/00_RESEARCH_SNAPSHOT.md`).
- Depends on the M5PM1 GPIO/amp control (issue 0001) for the SPK-amp/IR conflict.

**Acceptance criteria**
- IR TX produces an observable/decodable waveform; RX decodes a known remote.
- Evidence (logic-analyzer capture or receiver confirmation) saved under `evidence/`.
