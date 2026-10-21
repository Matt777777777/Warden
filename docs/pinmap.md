# WARDEN pin map (UNO R3)

Planned once for the whole project so nothing needs rewiring in Week 3.

| UNO pin | Used by | Week |
|---------|---------|------|
| A0 | Photoresistor (lid sensor) | 1 |
| A4 (SDA), A5 (SCL) | I2C bus: GY-521 motion sensor + DS3231 clock (they share the two wires) | 1 / 3 |
| D4 | LCD RS | 1 |
| D5 | LCD E | 1 |
| D6, D7, D8, A1 | LCD data D4, D5, D6, D7 | 1 |
| D9 | RFID RC522 RST | 3 |
| D10 | RFID RC522 SDA (SS) | 3 |
| D11, D12, D13 | RFID RC522 MOSI, MISO, SCK (SPI bus, fixed pins) | 3 |
| D2, D3 | HM-10 Bluetooth (software serial RX, TX) | 3 |
| A2 | Buzzer | 3 |
| A3 | Spare | |
| D0, D1 | Reserved: USB link to the laptop | |

## LCD 1602 wiring (pins in breadboard column j, K in row 30, VSS in row 45, screen overhanging to the right)

| Row | LCD pin | Name | Goes to |
|-----|---------|------|---------|
| 45 | 1 | VSS | GND rail |
| 44 | 2 | VDD | 5V rail |
| 43 | 3 | V0 (contrast) | potentiometer middle leg |
| 42 | 4 | RS | D4 |
| 41 | 5 | RW | GND rail (we only write to the screen) |
| 40 | 6 | E | D5 |
| 39-36 | 7-10 | D0-D3 | not connected (4-bit mode) |
| 35 | 11 | D4 | D6 |
| 34 | 12 | D5 | D7 |
| 33 | 13 | D6 | D8 |
| 32 | 14 | D7 | A1 |
| 31 | 15 | A (backlight +) | 220 Ω resistor (g31 to g27), then 5V rail |
| 30 | 16 | K (backlight -) | GND rail |

Potentiometer: outer legs to 5V and GND rails, middle leg to LCD pin 3.

## Final compact build (shield + mini breadboard, LCD and RFID on the lid)

Mini breadboard power: row 10 left = 5V bus, row 15 left = GND bus, row 16 right = 5V bus #2 (fed from b-10), row 17 right = GND bus #2 (fed from c-15). Backlight resistor 220 Ω from e-10 to e-13. Layout: `docs/compact_step2.png`.
No contrast potentiometer: V0 goes straight to GND (add a 1 kΩ resistor in series if the text is too dark).

### LCD 1602 on the lid (female end on the LCD pin, male end into...)

| LCD pin | Male end goes to |
|---------|------------------|
| VSS | GND bus #2 (g-17) |
| VDD | 5V bus #2 (g-16) |
| V0 | GND bus #2 (h-17) |
| RS | shield D4 |
| RW | GND bus #2 (i-17) |
| E | shield D5 |
| D0-D3 | nothing |
| D4 | shield D6 |
| D5 | shield D7 |
| D6 | shield D8 |
| D7 | shield A1 |
| A | breadboard a-13 (after the 220 Ω resistor) |
| K | GND bus #2 (j-17) |

### RFID RC522 on the lid (planned)

| RC522 pin | Male end goes to |
|-----------|------------------|
| 3.3V | 3.3V bus b-14 (never 5V) |
| RST | shield D9 |
| GND | shield GND |
| IRQ | nothing |
| MISO | shield D12 |
| MOSI | shield D11 |
| SCK | shield D13 |
| SDA | shield D10 |

12 + 7 = 19 of the kit's 20 male-female jumpers.

### 3.3V bus and DS3231 clock (step 3)

Row 14 left = 3.3V bus: a-14 from the shield 3.3V pin, b-14 RC522 3.3V, c-14 to the GY-521 AD0 row, e-14 DS3231 VCC.
GY-521 AD0 tied to 3.3V moves the motion sensor to I2C address 0x69, because the DS3231 uses 0x68.
I2C bus check (firmware/i2c_scan): 0x57 AT24C32 memory, 0x68 DS3231, 0x69 GY-521.

DS3231 (ZS-042) standing in column j, mounted reversed: GND j-9, VCC j-10, SDA j-11, SCL j-12, SQW j-13, 32K j-14.

| DS3231 pin | Wire |
|------------|------|
| SCL (row 12) | f-12 to b-3 |
| SDA (row 11) | f-11 to b-4 |
| VCC (row 10) | f-10 to e-14 (3.3V bus) |
| GND (row 9) | f-9 to e-15 (GND bus) |

**WARNING: the ZS-042 charges its coin cell. With a CR2032 (non-rechargeable) it must NEVER be powered from 5V.** At 3.3V no charge current flows.

LCD contrast: V0 to g-15, 1 kΩ resistor h-15 to h-17 (GND bus #2).
