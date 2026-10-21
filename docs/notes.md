# WARDEN build notes

## Light sensor baseline (Day 1)

Photoresistor on A0 (5V → photoresistor → A0 → 10 kΩ → GND), values 0–1023:

| Condition | Reading |
|-----------|---------|
| Ambient room light | 737 |
| Hand over sensor | 623 |
| Mug over sensor (simulated closed crate) | 40 |
| Phone torch | 975 |

Lid threshold set to 400: a closed crate (~40) and an open one (~737) sit far on either side, so detection cannot hesitate. A hand alone is not enough to read as "closed", which is fine: only a real lid makes it truly dark.

## LCD display (Day 1)

LCD 1602 working in 4-bit mode, shows the light level and LID: OPEN / CLOSED in real time (`firmware/lcd_light_test`). Wiring in `docs/pinmap.md`.

## Compact core (Day 2)

Final build started directly instead of a temporary rig. Prototype shield stacked on the UNO, 170-point mini breadboard stuck on the shield: GY-521 (pins in column e, rows 1-8, body over the trench), photoresistor + 10K divider, power buses on rows 10 (5V) and 15 (GND). Layout: `docs/compact_step1.png`. LCD and RFID reader will sit on the lid, linked with male-female jumpers. Crate must be opaque plastic: a metal can would block both Bluetooth and RFID (Faraday cage).

GY-521 first light: at rest az ≈ 0.93 g instead of 1.00 (factory offset of a low-cost sensor, |a| ≈ 0.93). Harmless for classification; a 6-face calibration could fix it in Week 2.

## Clock, badges and bus (Day 2)

RFID badges: white card 40 8C 7D A2 = AUTH (opens a 30 s window where opening is legitimate), blue key fob 05 24 8E B9 = INSPECTOR (shows the log on the LCD). The RC522 is a clone (version register 0x88 instead of 0x91/0x92), works the same.
DS3231 clock (ZS-042) added, with its AT24C32 4 KB memory used for the event log. I2C address clash found before wiring: MPU-6050 and DS3231 both default to 0x68. Fixed by tying GY-521 AD0 to 3.3V (sensor moves to 0x69). Bus scan confirms 0x57, 0x68, 0x69.
ZS-042 trickle-charges its coin cell; ours is a CR2032 (non-rechargeable), so the module runs on 3.3V only.

## Firmware v0.2 (rule-based baseline)

`firmware/warden` runs everything together: 100 Hz streaming (record.py compatible), lid detection with hysteresis, badge roles, DROP / SHOCK / FLIPPED rules, DS3231 timestamps, LCD status, inspector view, and a hash-chained log (CRC-16 per record over the previous chain value) in the AT24C32. `tools/verify_log.py` reads the log over USB and flags any edited or deleted record. Known limit: CRC-16 stops naive edits, not an attacker who knows the algorithm; production would use an HMAC with a key in a secure element.

## Sensor orientation in the crate

(Day 4: which face of the crate points along +X, +Y, +Z?)

## Observations per class

(Day 7: which classes look easy to separate, which look alike, and why?)

## Problems and fixes

- **LCD backlight on but no text.** VDD (LCD pin 2) was wired to GND instead of 5V, so both power pins of the display logic sat on ground while the backlight, a separate circuit, still lit up. Isolated by forcing the contrast pin (V0) to GND to rule out the potentiometer, then checking the power pins. Fixed by moving VDD to the 5V rail. No damage: two pins on the same ground is not a short circuit.
- **Recordings were 2.8 s too long (1,779 samples for 15 s).** On macOS, `reset_input_buffer()` did not discard the samples buffered during the 3-2-1 countdown, so calm pre-GO data was being saved under the session label: a silent labelling error that would have poisoned the training set. Caught because the sample count did not match 15 s × 100 Hz. Fixed in `record.py` by draining the backlog after GO and trimming to the last N seconds of the board's own clock. Verified: 1,499 samples for 15 s.
- **Light stuck near 1000 on the compact core.** The photoresistor legs, folded flat to save space, were touching, so current bypassed the sensor and A0 read 5V whatever the light. Isolated by removing the photoresistor: A0 dropped to 0, which cleared the A0 wire and the 10K resistor. Fixed by mounting the photoresistor upright with straight legs (it now faces the lid, which is the right orientation anyway).
- **Arduino Serial Plotter unreadable.** It draws every column on one axis, so `t_ms` (100,000+) flattens the motion signals (±1 g). Use `tools/plot.py` instead, which gives each signal its own scale.
- **"Resource busy" when starting record.py.** The IDE's Serial Monitor tab still held the port. Only one program can use it at a time; `record.py` now prints a clear message instead of a traceback.
- **macOS blocked Terminal from reading ~/Documents** ("Operation not permitted"). Fixed by granting Terminal access in System Settings › Privacy & Security › Files and Folders.
