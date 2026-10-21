// WARDEN - I2C scanner: lists every device answering on the SDA/SCL bus
// Expected on WARDEN: 0x57 (clock's memory chip), 0x68 (DS3231 clock), 0x69 (GY-521 motion sensor)
#include <Wire.h>

void setup() {
  Serial.begin(115200);
  Wire.begin();
  delay(500);
}

void loop() {
  Serial.println(F("Scanning I2C bus..."));
  int found = 0;
  for (uint8_t address = 1; address < 127; address++) {
    Wire.beginTransmission(address);          // "is anybody at this address?"
    if (Wire.endTransmission() == 0) {        // 0 = somebody answered
      Serial.print(F("  device at 0x"));
      if (address < 16) Serial.print('0');
      Serial.print(address, HEX);
      if (address == 0x57) Serial.print(F("  -> AT24C32 memory (on the clock module)"));
      if (address == 0x68) Serial.print(F("  -> DS3231 clock"));
      if (address == 0x69) Serial.print(F("  -> GY-521 motion sensor (AD0 high)"));
      Serial.println();
      found++;
    }
  }
  Serial.print(found);
  Serial.println(F(" device(s) found.\n"));
  delay(5000);                                // scan again every 5 seconds
}
