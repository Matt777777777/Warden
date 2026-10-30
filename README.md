# WARDEN

**A low-cost, tamper-evident black box for military equipment crates.**

WARDEN rides inside a crate, feels every movement it goes through, knows when the lid is opened and by whom, and keeps a timestamped log that cannot be edited without it showing. An inspector reads the crate's history on its screen or from a phone, and the phone checks the log's integrity on the spot.

Built on an Arduino UNO with **2 KB of RAM**, from starter-kit parts, without soldering.

<p align="center">
  <img src="docs/media/app-status.png" width="250" alt="Crate status in the phone app">
  <img src="docs/media/app-log-verified.png" width="250" alt="Log downloaded and verified">
  <img src="docs/media/app-log-forged.png" width="250" alt="A forged record is detected">
</p>
<p align="center"><sub>Inspector app: live crate status · log downloaded and verified · a forged record detected</sub></p>

## Why

Countries supplying military equipment to partners are expected to verify where it ends up (*end-use monitoring*). In practice this relies on paperwork, serial-number checks and occasional inspections: between two inspections, nobody knows what happened to a crate. WARDEN explores a different answer: let the crate keep its own record, cheaply enough to put one in every box.

## What it does

| Layer | Function | How |
|-------|----------|-----|
| **Sense** | Motion and lid state | MPU-6050 accelerometer + gyroscope at 100 Hz, photoresistor inside the lid |
| **Think** | Recognise what is happening | Physical rules for drops and flips; a random forest running on the UNO itself for forced entry |
| **Prove** | Who, when, authorised or not | RFID badges, real-time clock, hash-chained event log in non-volatile memory |
| **Report** | Tell a human | 16×2 LCD on the crate, phone app over Wi-Fi relay or Bluetooth LE |

![Architecture](docs/media/architecture.png)

**Events recorded:** power on, authorised badge, inspector badge, unknown badge, lid opened with or without authorisation, lid closed, drop (with impact force), forced entry (with the model's confidence), flipped, upright, log cleared.

**Authorisation model:** the authorised badge allows exactly one lid opening within 30 seconds. Any other opening is logged as an alert. The inspector badge scrolls the most recent events on the crate's screen.

## On-device machine learning

Telling someone forcing a crate apart from someone carrying it is not something a threshold can do: both shake the box, both produce hits above 3 g. WARDEN learns the difference from data and makes the call on the UNO itself, with no network.

**Data.** 90 labelled sessions of 20 s recorded in the final crate (still, carry, open, flip, drop, tamper; 15 each), 3,510 one-second windows. **Features.** 17 statistics of acceleration and rotation computed on the fly from running sums (`firmware/warden/features.c`), so a window costs about 60 bytes of RAM, never a buffer of raw samples. The *same C file* is compiled into the training pipeline, so the model learns on exactly the numbers the crate computes. **Model.** A 10-tree random forest (536 nodes) exported to C arrays in flash (`model.h`, about 5 KB), giving a verdict every 0.5 s in 1.2 ms.

**Results** (5-fold cross-validation grouped by session: the model is always tested on sessions it never saw):

| Session type | v0.3 rule "SHOCK > 3 g" | v0.4 model + lid sensor |
|---|---|---|
| Forced entry (tamper) | 47 % detected | **100 % detected** |
| Normal carrying | 47 % false alarms | 13 % false alarms |
| Authorised opening, flip, drop, still | 0 to 20 % false alarms | 0 % false alarms |

Drops stay with the physical rule (free fall then impact), which already catches 100 % of them; flips stay with the orientation rule. The model only does what rules could not. A forced-entry alert needs 8 of the 10 trees to agree in 3 of the last 4 windows, and is ignored while the light sensor sees the lid open (openings are judged by the badge rule). The C export is checked against scikit-learn on every window of the dataset, on the laptop and on a simulated ATmega328P: identical votes ([tools/parity](tools/parity/README.md)). Full report: **[docs/ml/report.md](docs/ml/report.md)**.

**Limits.** No road-transport data: in a vehicle the model will see vibrations it has never learnt, so this is the next dataset to collect. One crate, one operator, one day of recordings: performance on other crates is not established.

## Inspection report by a local language model

At the end of a shipment the inspector taps *Draft inspection report* in the phone app. The Mac reads the crate's log, re-verifies the hash chain, and a small open-source language model running **offline on the laptop** (Llama 3.2 through [Ollama](https://ollama.com), no cloud, nothing leaves the room) drafts the report in English: summary, what each incident may indicate, recommended actions. The report shows in the app and is saved as a Word document with the full event log and a signature line.

The model is the clerk, not the investigator. Facts, durations and the risk level (LOW, MEDIUM, HIGH) are computed by plain code from the verified log; the model only writes the prose, in a fixed JSON structure. Every time and figure it writes is then checked against the log, and a sentence that cannot be matched (an invented time, a wrong impact force) is removed and listed. Without a model installed, a template is used and the report says so.

```bash
ollama pull llama3.2:3b                 # once, about 2 GB
python tools/report.py --demo           # sample shipment, no crate needed
python tools/report.py                  # the real crate on USB
```

## Tamper-evident log

Each record (time, event, value) is stored with a 16-bit *chain* value computed over the previous chain value and the record itself. Editing, deleting or reordering any record breaks every link after it, the way a page torn out of a numbered, glued logbook leaves a gap. The log lives in the 4 KB AT24C32 memory of the clock module and survives power loss; a reboot is itself recorded.

The chain is verified independently of the crate, by `tools/verify_log.py` on a laptop or inside the phone app.

**Known limitation.** CRC-16 detects accidental damage and naive edits, not an attacker who knows the algorithm and can recompute the chain. A production design would use a keyed MAC (e.g. HMAC-SHA256) with the key held in a secure element (e.g. ATECC608), so that only the crate can produce valid records. This is the first item on the security roadmap.

## Hardware

Arduino UNO R3, prototype shield with a 170-point mini breadboard, GY-521 (MPU-6050), photoresistor, RC522 RFID reader, DS3231 real-time clock with AT24C32 memory, LCD 1602. Bill of materials, pin map and wiring: **[docs/hardware.md](docs/hardware.md)**.

## Repository layout

```
firmware/warden/            main firmware (v0.4): warden.ino, features.c (shared with training), model.h (generated)
firmware/diagnostics/       one-purpose test sketches: I2C scan, motion stream, LCD, RFID
app/index.html              phone inspector app (single file, no dependencies)
tools/record.py             record labelled motion sessions for the training dataset
tools/train.py              train the classifier, evaluate it against the rules, export model.h
tools/check_parity.py       check that the C export votes exactly like scikit-learn
tools/watch_ai.py           watch the crate's AI decide, live (verdicts and events only)
tools/report.py             inspection report: verified facts + local language model, Word output
tools/plot.py               plot a recorded session
tools/verify_log.py         download the crate's log over USB and verify its integrity
tools/bridge.py             relay between the crate (USB) and the phone app (Wi-Fi)
docs/hardware.md            bill of materials, pin map, wiring
docs/engineering-log.md     dated build log: measurements, decisions, bugs and fixes
docs/ml/                    training report and confusion matrix
data/raw/                   training dataset (labelled CSV sessions)
```

## Getting started

**Firmware.** Open `firmware/warden/warden.ino` in the Arduino IDE, install the *MFRC522* library (Library Manager, by GithubCommunity), select *Arduino Uno* and upload. On first boot the clock is set to the compile time. Over the serial port (115200 baud) the crate accepts `dump`, `clear` and `time YYYY-MM-DD HH:MM:SS`.

**Laptop tools.** Python 3.9+:

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
python tools/verify_log.py                      # read and verify the crate's log
python tools/record.py --label drop --seconds 20
python tools/plot.py --latest
python tools/train.py                           # retrain and regenerate firmware/warden/model.h
python tools/check_parity.py
```

**Phone app.** Two ways to reach the crate, same app and protocol:

- *Via a Mac (works today):* run `python tools/bridge.py` with the crate on USB, then open the printed address in Safari on an iPhone connected to the same Wi-Fi.
- *Bluetooth LE (HM-10 module, in progress):* open the [hosted app](https://matt777777777.github.io/Warden/app/) in the Bluefy browser on iPhone (Safari has no Web Bluetooth).

A demo mode simulates a crate, including a forged record, without any hardware.

## Serial protocol

| Direction | Line | Meaning |
|-----------|------|---------|
| crate → host | `t_ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,light` | 100 Hz sensor stream |
| crate → host | `#S,<alerts>,<lid>,<auth s>,<unix>` | status, every 2 s |
| crate → host | `#E <date> <time> <EVENT> [<g>] [ALERT]` | live event |
| crate → host | `#M,<class>,<votes still>,<handling>,<drop>,<tamper>,<lid>,<µs>` | model verdict, every 0.5 s (class 0 still, 1 handling, 2 drop, 3 tamper) |
| crate → host | `#L,<seq>,<unix>,<datetime>,<event>,<value>,<chain>` | log dump, ends with `#L,END,<count>,<chain>` |
| host → crate | `dump` · `time YYYY-MM-DD HH:MM:SS` · `clear` | commands (`clear` is not relayed to the phone) |

## Roadmap

- [x] Motion and light sensing, 100 Hz streaming and labelled recording tools
- [x] Compact solder-free build: clock, RFID, LCD, shared I2C bus
- [x] Rule-based detection, badge authorisation, hash-chained log, integrity verifier
- [x] Phone inspector app with Wi-Fi relay and demo mode
- [x] Final enclosure (opaque plastic crate) and labelled dataset collection (90 sessions)
- [x] On-device classifier (features computed on the fly, random forest exported to C), benchmarked against the rules
- [ ] Road-transport dataset (vehicle vibrations)
- [x] Inspection report drafted by a local language model on the inspector's laptop, fact-checked against the verified log
- [ ] Bluetooth LE link (HM-10) and battery operation
- [ ] Keyed MAC instead of CRC for the log chain

## Scope

WARDEN is a defensive, educational prototype focused on accountability and arms-control verification. It is not a certified security product.

## License

MIT. See [LICENSE](LICENSE).
