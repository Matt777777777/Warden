# WARDEN

**A €50 AI black box that tells you what happened to a weapons crate between the factory and the front line.**

Western countries have shipped billions of euros of military equipment to partners, and verifying where it ends up ("end-use monitoring") is a real political problem. Today that verification relies mostly on paperwork, serial-number scans and occasional inspections. WARDEN explores a different idea: a small tamper-evident sensor that rides inside the crate, feels every movement it goes through, and uses a tiny machine-learning model running *on the microcontroller itself* to classify those movements (at rest, carried, in a vehicle, dropped, flipped, tampered with, opened). Authorised openings are signed with an RFID badge; everything else is logged with a timestamp.

The constraint is the point: the whole thing runs on an Arduino UNO R3 with **2 KB of RAM**.

## Status

| Week | Goal | Status |
|------|------|--------|
| 1 | Sensor streaming + labelled dataset | In progress |
| 2 | Feature engineering, model training, model running on the UNO | Planned |
| 3 | RFID authorisation, timestamped tamper-evident log, phone readout, enclosure, demo video | Planned |

## How it works

```
 GY-521 IMU ──┐                        ┌── RFID badge (authorised opening)
 Light sensor ┼─> features ─> tiny ML ─┼── DS3231 clock (timestamps)
              │   (on UNO)    model    └── event log ─> LCD / buzzer / phone (BLE)
```

## Hardware (Week 1)

Elegoo UNO R3, GY-521 (MPU-6050) accelerometer + gyroscope, photoresistor + 10 kΩ resistor, breadboard, jumper wires. Wiring: `docs/wiring_week1.png`.

## Quick start

1. Open `firmware/imu_stream/imu_stream.ino` in the Arduino IDE, select **Arduino Uno**, upload.
2. Install the Python tools: `pip install -r requirements.txt`
3. Record a session: `python tools/record.py --label still --seconds 15`
4. Look at it: `python tools/plot.py --latest`

## Repository layout

```
firmware/imu_stream/   Arduino sketch: streams IMU + light at 100 Hz over USB
tools/record.py        records one labelled session to data/raw/
tools/plot.py          plots a session
data/raw/              recorded sessions (CSV) + sessions_log.csv
docs/                  wiring diagrams, notes, media
```

## Event classes

| Label | Meaning |
|-------|---------|
| `still` | crate at rest |
| `carry` | carried by a person |
| `vehicle` | transported in a vehicle |
| `drop` | free fall then impact |
| `flip` | tilted past 90° / turned upside down |
| `tamper` | prying, hitting, violent shaking |
| `open` | lid opened (detected by the light sensor, rule-based) |

## Disclaimer

Educational, defensive prototype focused on accountability and arms-control verification. Not a certified security device.
