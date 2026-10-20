// WARDEN - LCD test: show the light level and the lid state on the screen
#include <LiquidCrystal.h>   // comes with the Arduino IDE, nothing to install

// LCD pins in this order: RS, E, D4, D5, D6, D7  (see docs/pinmap.md)
LiquidCrystal lcd(4, 5, 6, 7, 8, A1);

const int LIGHT_PIN = A0;
const int LID_THRESHOLD = 400;   // below = dark = lid closed (we will tune it)

void setup() {
  lcd.begin(16, 2);              // the screen has 16 characters x 2 lines
  lcd.print("WARDEN v0.1");
  lcd.setCursor(0, 1);           // column 0, line 1 (the second line)
  lcd.print("Booting...");
  delay(1500);
  lcd.clear();
}

void loop() {
  int light = analogRead(LIGHT_PIN);

  lcd.setCursor(0, 0);
  lcd.print("Light: ");
  lcd.print(light);
  lcd.print("    ");             // spaces erase leftover digits (e.g. 1000 -> 999)

  lcd.setCursor(0, 1);
  if (light < LID_THRESHOLD) {
    lcd.print("LID: CLOSED     ");
  } else {
    lcd.print("LID: OPEN !     ");
  }
  delay(200);                    // refresh 5 times per second
}
