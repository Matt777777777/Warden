// WARDEN - RFID test: read a badge and show its ID on the LCD and on the laptop
// Needs the "MFRC522" library (Library Manager, author GithubCommunity).
#include <SPI.h>
#include <MFRC522.h>
#include <LiquidCrystal.h>

const uint8_t RFID_SS  = 10;   // RC522 pin "SDA"
const uint8_t RFID_RST = 9;    // RC522 pin "RST"

MFRC522 rfid(RFID_SS, RFID_RST);
LiquidCrystal lcd(4, 5, 6, 7, 8, A1);   // RS, E, D4, D5, D6, D7 (see docs/hardware.md)

void showWaiting() {
  lcd.clear();
  lcd.print("WARDEN  RFID");
  lcd.setCursor(0, 1);
  lcd.print("Scan a badge...");
}

void setup() {
  Serial.begin(115200);
  lcd.begin(16, 2);
  SPI.begin();               // SPI = the fast 4-wire "conversation" the reader uses
  rfid.PCD_Init();
  delay(10);

  // Ask the reader for its version: 0x00 or 0xFF means nobody answered (wiring problem).
  byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.print(F("# RC522 version: 0x"));
  Serial.println(version, HEX);
  if (version == 0x00 || version == 0xFF) {
    lcd.print("RFID not found");
    lcd.setCursor(0, 1);
    lcd.print("Check wiring");
    Serial.println(F("# ERROR: RC522 not responding. Check SDA->D10, SCK->D13, MOSI->D11, MISO->D12, 3.3V, GND."));
    while (true) {}
  }
  showWaiting();
}

void loop() {
  // Is a badge close to the reader, and can we read its ID?
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  // Every badge has a unique ID (UID), usually 4 bytes: show them in hexadecimal.
  lcd.clear();
  lcd.print("Badge ID:");
  lcd.setCursor(0, 1);
  Serial.print(F("UID:"));
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) { lcd.print('0'); }
    lcd.print(rfid.uid.uidByte[i], HEX);
    if (i < rfid.uid.size - 1) lcd.print(' ');
    Serial.print(' ');
    if (rfid.uid.uidByte[i] < 0x10) Serial.print('0');
    Serial.print(rfid.uid.uidByte[i], HEX);
  }
  Serial.println();

  rfid.PICC_HaltA();         // tell the badge we are done with it
  delay(2000);
  showWaiting();
}
