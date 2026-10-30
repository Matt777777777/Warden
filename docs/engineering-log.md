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

## Dataset collection (30 Oct 2026)

90 sessions of 20 s recorded in the final opaque crate with `tools/record.py`, 15 per class: still, open, carry, flip, drop, tamper. Vehicle sessions were left out on purpose (no safe way to record them that day); this is documented as a limit rather than faked with shaking by hand. One drop session with 18 gaps after a 13 g impact was discarded (loose sensor wire, re-seated).

## On-device machine learning (30 Oct 2026)

Pipeline in `tools/train.py`. Key decisions:
- **One feature code base.** `firmware/warden/features.c` is compiled into the firmware and loaded in Python through ctypes. Training and inference cannot drift apart.
- **Two staggered windows** of 1 s (running sums only, ~60 bytes each) give a verdict every 0.5 s without storing samples.
- **Label cleaning.** A session label says what happened at some point in 20 s, not in every second. Calm windows are relabelled "still"; in drop sessions, active windows with no free fall and no impact (lifting the crate before letting go) are excluded. Open, carry and flip are grouped as "handling".
- **Evaluation grouped by session** (no window of a test session is ever seen in training), and at alert level against the v0.3 rules replayed on the same raw data.
- **Hybrid design.** Drops stay with the physical rule (100 % detected), flips with the orientation rule; the model only judges forced entry vs handling. Tamper alerts are muted while the lid is open: an authorised opening gesture looks a lot like forcing, and the badge rule already judges openings (open-session false alarms 20 % → 0 %).
- Tried and dropped: extra features (`jerk_max`, `sharp_frac`) gave no real gain but are cheap so they stayed; a 3-window majority vote smoothed out short events and hurt.

Result: forced entry 100 % detected vs 47 % for the SHOCK rule, false alarms on carrying 13 % vs 47 %, 0 % on the other classes.

**Export and parity.** The forest is written as flat arrays in flash. Thresholds are rounded *down* to float32 so that `x <= T` on the crate gives the same answer as scikit-learn's double-precision comparison. Checked on every window of the dataset (3,510) on the laptop, then on a simulated ATmega328P (simavr, 180 windows from 12 sessions), whose software floating point could in principle differ: identical votes everywhere.

## Firmware v0.4, on-device AI (30 Oct 2026)

TAMPER event (code 13) raised by the model; the SHOCK rule is retired (code 6 kept so older logs still decode). Live verdict line `#M` every 0.5 s. I2C timeout and sensor recovery ported from `imu_stream` v0.2. Measured: 28.1 KB of flash (87 % of the 32 KB available to sketches, +8.5 KB vs v0.3), 886 bytes of static RAM (+146), 0.62 ms per sample to update the features and 1.2 ms per inference, inside the 10 ms sampling period.

## First run on the real crate (30 Oct 2026)

Firmware v0.4 flashed, watched with `tools/watch_ai.py`. Inference measured on the chip: 0.9 to 1.1 ms (simulation predicted up to 1.2 ms). Forced entry: tamper votes climbed 4, 6, 3, 4, 8, 9, 8 and the alert fired about 3 s after the attack began. Opening the lid without a badge: OPEN !! alert, tamper votes ignored while the lid was open, as designed. Crate set down hard from about 4 cm: the model said "drop" but no DROP alert, correctly (no free fall, and only the physical rule raises drops). Carrying: one window at 8/10, then 8, 9, 6, one window short of a false alarm, so the margin is thin and carrying remains the case to watch. At rest: 0/10 throughout.

## Inspection report with a local language model (30 Oct 2026)

`tools/report.py`, also reachable from the phone app through the Mac relay. Constraint: the inspector's laptop is a 2020 MacBook Air (Intel Core i3, 8 GB, no usable GPU), and the report must be produced offline. Choice: Ollama with Llama 3.2 3B (1B as the fast fallback), run on the CPU.
Design rule: the language model never decides anything. Code verifies the chain, extracts the facts (incident list, durations, what preceded what, power interruptions) and sets the risk level by fixed rules. The model receives those facts and returns prose in a JSON schema enforced by Ollama, so the output always parses. Code then checks every time (HH:MM), impact force (g) and tree count it wrote against the log; unsupported sentences are removed and listed in the report. Tested against a mock model that deliberately invented a time and an impact force: both sentences were caught and removed.
First runs of the real model on the Mac: 115 to 183 s per report at about 4 tokens/s. Reading its output exposed what the checker missed. (1) Counts: it wrote "multiple unauthorized openings" and "repeated instances" when each event happened once, because it read "(1x)" as "several"; counts are now written in words and plurals that do not match the log are rejected. (2) Generic prose and a missed link: it called the restart a "power management" fault instead of seeing forced entry, unauthorised opening and power cut as one sequence; code now detects linked sequences and the prompt carries a short glossary of what each event means. (3) Contradicting the verdict: "further investigation is not required" in a HIGH-risk report, and doubts about a log whose chain was verified; such sentences are now rejected, because the verdict belongs to the rules. Internal incident ids in the prose are replaced by times.

## Problems found and how they were fixed

- **LCD backlight on but no text.** VDD (LCD pin 2) was wired to GND instead of 5V, so both power pins of the display logic sat on ground while the backlight, a separate circuit, still lit up. Isolated by forcing the contrast pin (V0) to GND to rule out the potentiometer, then checking the power pins. Fixed by moving VDD to the 5V rail. No damage: two pins on the same ground is not a short circuit.
- **Recordings were 2.8 s too long (1,779 samples for 15 s).** On macOS, `reset_input_buffer()` did not discard the samples buffered during the 3-2-1 countdown, so calm pre-GO data was being saved under the session label: a silent labelling error that would have poisoned the training set. Caught because the sample count did not match 15 s × 100 Hz. Fixed in `record.py` by draining the backlog after GO and trimming to the last N seconds of the board's own clock. Verified: 1,499 samples for 15 s.
- **Light stuck near 1000 on the compact core.** The photoresistor legs, folded flat to save space, were touching, so current bypassed the sensor and A0 read 5V whatever the light. Isolated by removing the photoresistor: A0 dropped to 0, which cleared the A0 wire and the 10K resistor. Fixed by mounting the photoresistor upright with straight legs (it now faces the lid, which is the right orientation anyway).
- **Firmware update silently reverted.** The Arduino IDE auto-saves the open tab: with warden.ino open, it wrote the old v0.2 over the new v0.3 file, so the phone got events but no status line. Rule: close the sketch tab (or quit the IDE) before replacing a .ino file from outside the IDE.
- **Compile error "DateTime does not name a type".** The IDE inserts function prototypes above the first function; a struct used in prototypes must be declared before any function.
- **Arduino Serial Plotter unreadable.** It draws every column on one axis, so `t_ms` (100,000+) flattens the motion signals (±1 g). Use `tools/plot.py` instead, which gives each signal its own scale.
- **"Resource busy" when starting record.py.** The IDE's Serial Monitor tab still held the port. Only one program can use it at a time; `record.py` now prints a clear message instead of a traceback.
- **Clock module wobbling blocked the I2C bus.** During drop sessions the DS3231 board, held only by its pins, moved enough to flicker its power LED, and the whole bus froze ("No data header received"). Removed for the collection; it goes back in held by hot glue or tape.
- **Sensor freezes after hard impacts, even without the clock.** The Arduino Wire library waits forever for a device that stops answering. Fixed with `Wire.setWireTimeout(3000, true)`, a re-initialisation after 5 failed reads, and a once-per-second check of the sensor's sleep bit (a power glitch restarts it asleep). `record.py` now warns live when data stops and flags sessions with gaps.
- **Generated model did not compile as C.** The exporter wrote zero thresholds as `0f`, which is not a valid C literal. Caught by compiling `model.h` in the parity harness with `-Wall -Werror` before it reached the board; now written as `0.0f`.
- **macOS blocked Terminal from reading ~/Documents** ("Operation not permitted"). Fixed by granting Terminal access in System Settings › Privacy & Security › Files and Folders.
