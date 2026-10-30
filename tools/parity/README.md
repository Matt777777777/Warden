# Parity checks

Three things must agree for the crate to behave like the model evaluated in `docs/ml/report.md`:
scikit-learn on the laptop, the exported C code compiled for the laptop, and the same C code
running on the ATmega328P.

**Laptop: Python vs C** (any machine with a C compiler):

```bash
python tools/train.py
python tools/check_parity.py      # compiles harness.c with features.c + model.h, replays every session
```

Result on the 90-session dataset: 3,510 windows, identical votes from all 10 trees in 100 % of them.

**Chip: laptop C vs ATmega328P** (needs `avr-gcc` and `simavr`; Linux `apt install gcc-avr avr-libc simavr`, macOS `brew tap osx-cross/avr && brew install avr-gcc simavr`). The AVR has its own float library (no FPU), so this checks that it computes the same numbers:

```bash
cd build && python ../tools/parity/make_session_h.py ../data/raw/tamper_20261030-121738.csv 800
avr-gcc -mmcu=atmega328p -DF_CPU=16000000L -Os -I../firmware/warden -I. -o bench.elf \
        ../tools/parity/avr_bench.c ../firmware/warden/features.c -lm
simavr -m atmega328p -f 16000000 bench.elf     # prints "W <window> <votes x4>" then "T <cycles>"
./parity_harness trunc.csv                      # same votes expected
```

Result on 12 sessions (2 per class, 180 windows): identical votes on every window. Cost measured on the
simulated chip at 16 MHz: about 9,900 cycles (0.62 ms) per sample to update both windows, and at most
19,200 cycles (1.2 ms) to compute the 17 features and run the 10 trees, twice per second.
