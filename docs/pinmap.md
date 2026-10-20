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
