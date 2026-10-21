# WARDEN engineering log

A dated record of build decisions, measurements and the bugs found along the way.

## Light sensor baseline (20 Oct 2026)

Photoresistor on A0 (5V → photoresistor → A0 → 10 kΩ → GND), values 0–1023:

| Condition | Reading |
|-----------|---------|
| Ambient room light | 737 |
| Hand over sensor | 623 |
| Mug over sensor (simulated closed crate) | 40 |
| Phone torch | 975 |

Lid threshold set to 400: a closed crate (~40) and an open one (~737) sit far on either side, so detection cannot hesitate. A hand alone is not enough to read as "closed", which is fine: only a real lid makes it truly dark.

## LCD display (20 Oct 2026)

LCD 1602 working in 4-bit mode, shows the light level and LID: OPEN / CLOSED in real time (`firmware/diagnostics/lcd_light_test`). Wiring in `docs/hardware.md`.

## Compact core (21 Oct 2026)

Final build started directly instead of a temporary rig. Prototype shield stacked on the UNO, 170-point mini breadboard stuck on the shield: GY-521 (pins in column e, rows 1-8, body over the trench), photoresistor + 10K divider, power buses on rows 10 (5V) and 15 (GND). Layout: `docs/media/build-step-1.png`. LCD and RFID reader will sit on the lid, linked with male-female jumpers. Crate must be opaque plastic: a metal can would block both Bluetooth and RFID (Faraday cage).

GY-521 first light: at rest az ≈ 0.93 g instead of 1.00 (factory offset of a low-cost sensor, |a| ≈ 0.93). Harmless for classification; a 6-face calibration could fix it in Week 2.

## Clock, badges and bus (21 Oct 2026)

RFID badges: white card 40 8C 7D A2 = AUTH (opens a 30 s window where opening is legitimate), blue key fob 05 24 8E B9 = INSPECTOR (shows the log on the LCD). The RC522 is a clone (version register 0x88 instead of 0x91/0x92), works the same.
DS3231 clock (ZS-042) added, with its AT24C32 4 KB memory used for the event log. I2C address clash found before wiring: MPU-6050 and DS3231 both default to 0x68. Fixed by tying GY-521 AD0 to 3.3V (sensor moves to 0x69). Bus scan confirms 0x57, 0x68, 0x69.
ZS-042 trickle-charges its coin cell; ours is a CR2032 (non-rechargeable), so the module runs on 3.3V only.

## Firmware v0.2 → v0.3, rule-based baseline (21 Oct 2026)

`firmware/warden` runs everything together: 100 Hz streaming (record.py compatible), lid detection with hysteresis, badge roles, DROP / SHOCK / FLIPPED rules, DS3231 timestamps, LCD status, inspector view, and a hash-chained log (CRC-16 per record over the previous chain value) in the AT24C32. `tools/verify_log.py` reads the log over USB and flags any edited or deleted record. Known limit: CRC-16 stops naive edits, not an attacker who knows the algorithm; production would use an HMAC with a key in a secure element.

## Phone app (21 Oct 2026)

`app/index.html`: single-file inspector web app (no dependencies), hosted on GitHub Pages. Status card (SEALED / ALERT, lid, crate clock, authorisation countdown), live events, read + verify the hash chain in the phone, sync the crate clock, export CSV. Demo mode with a "forge a record" button that shows the integrity check failing.
Two transports, one protocol (`#S` status every 2 s, `#E` events, `#L` log dump):
- Bluetooth LE through the HM-10 (Web Bluetooth, needs the Bluefy browser on iPhone), pending the module.
- Mac relay (`tools/bridge.py`): the crate stays on USB, the Mac relays its messages to the phone over Wi-Fi with Server-Sent Events. Works in Safari. The relay only forwards `dump` and `time`: the log cannot be erased from the phone.
iOS does not let web pages or ordinary apps talk to a USB serial device, hence the relay.

## Problems found and how they were fixed

- **LCD backlight on but no text.** VDD (LCD pin 2) was wired to GND instead of 5V, so both power pins of the display logic sat on ground while the backlight, a separate circuit, still lit up. Isolated by forcing the contrast pin (V0) to GND to rule out the potentiometer, then checking the power pins. Fixed by moving VDD to the 5V rail. No damage: two pins on the same ground is not a short circuit.
- **Recordings were 2.8 s too long (1,779 samples for 15 s).** On macOS, `reset_input_buffer()` did not discard the samples buffered during the 3-2-1 countdown, so calm pre-GO data was being saved under the session label: a silent labelling error that would have poisoned the training set. Caught because the sample count did not match 15 s × 100 Hz. Fixed in `record.py` by draining the backlog after GO and trimming to the last N seconds of the board's own clock. Verified: 1,499 samples for 15 s.
- **Light stuck near 1000 on the compact core.** The photoresistor legs, folded flat to save space, were touching, so current bypassed the sensor and A0 read 5V whatever the light. Isolated by removing the photoresistor: A0 dropped to 0, which cleared the A0 wire and the 10K resistor. Fixed by mounting the photoresistor upright with straight legs (it now faces the lid, which is the right orientation anyway).
- **Firmware update silently reverted.** The Arduino IDE auto-saves the open tab: with warden.ino open, it wrote the old v0.2 over the new v0.3 file, so the phone got events but no status line. Rule: close the sketch tab (or quit the IDE) before replacing a .ino file from outside the IDE.
- **Compile error "DateTime does not name a type".** The IDE inserts function prototypes above the first function; a struct used in prototypes must be declared before any function.
- **Arduino Serial Plotter unreadable.** It draws every column on one axis, so `t_ms` (100,000+) flattens the motion signals (±1 g). Use `tools/plot.py` instead, which gives each signal its own scale.
- **"Resource busy" when starting record.py.** The IDE's Serial Monitor tab still held the port. Only one program can use it at a time; `record.py` now prints a clear message instead of a traceback.
- **macOS blocked Terminal from reading ~/Documents** ("Operation not permitted"). Fixed by granting Terminal access in System Settings › Privacy & Security › Files and Folders.
