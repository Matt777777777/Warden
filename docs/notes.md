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

## Sensor orientation in the crate

(Day 4: which face of the crate points along +X, +Y, +Z?)

## Observations per class

(Day 7: which classes look easy to separate, which look alike, and why?)

## Problems and fixes

- **LCD backlight on but no text.** VDD (LCD pin 2) was wired to GND instead of 5V, so both power pins of the display logic sat on ground while the backlight, a separate circuit, still lit up. Isolated by forcing the contrast pin (V0) to GND to rule out the potentiometer, then checking the power pins. Fixed by moving VDD to the 5V rail. No damage: two pins on the same ground is not a short circuit.
- **GY-521 header pins not soldered.** Press-fit pins would disconnect on every shock, which would corrupt drop and tamper data. Soldering required before Day 2.
- **macOS blocked Terminal from reading ~/Documents** ("Operation not permitted"). Fixed by granting Terminal access in System Settings › Privacy & Security › Files and Folders.
