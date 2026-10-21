/*
  WARDEN firmware v0.3 - the AI black box for arms shipments (rule-based edition)
  ===========================================================================
  What runs here, all at once:
    SENSE   GY-521 motion sensor + photoresistor, sampled 100 times per second,
            streamed to the laptop in the same CSV format as imu_stream
            (so tools/record.py keeps working for dataset collection).
    THINK   Hand-written rules (the baseline the AI will have to beat in Week 2):
            DROP    free fall (|a| < 0.35 g for 60 ms) followed by an impact > 2 g
            SHOCK   violent hit > 3 g without a free fall (knock, blow, forced entry)
            FLIPPED crate upside down for more than 1 s
    PROVE   RFID badges: AUTH badge opens a 30 s window in which opening the lid
            is legitimate; INSPECTOR badge shows the log on the screen.
            Every event is timestamped by the DS3231 clock and written to the
            clock module's 4 KB memory (AT24C32) as a hash chain: each record
            carries a fingerprint of the previous one, so deleting or editing a
            record breaks the chain and shows (tools/verify_log.py).
    REPORT  16x2 LCD: status, time, last event, authorisation countdown.

  Status line every 2 s for the phone app: #S,<alerts>,<lid>,<auth s left>,<unix>

  Serial commands (115200 baud, end with Enter):
    dump                          print the whole log
    clear                         erase the log (development only)
    time YYYY-MM-DD HH:MM:SS      set the clock

  Libraries: Wire, SPI, LiquidCrystal (built in) + MFRC522 (Library Manager).
  Pin map: docs/pinmap.md
*/

#include <Wire.h>
#include <SPI.h>
#include <LiquidCrystal.h>
#include <MFRC522.h>

// ------------------------------------------------------------------ settings
const unsigned long SAMPLE_PERIOD_US = 10000;   // 100 Hz
const int   LIGHT_OPEN   = 400;                 // light above this = lid open
const int   LIGHT_CLOSED = 300;                 // below this = lid closed (gap = hysteresis)
const unsigned long AUTH_WINDOW_MS = 30000;     // how long an AUTH badge authorises opening

const float FREEFALL_G   = 0.35;                // |a| below this = falling
const uint8_t FREEFALL_MIN_SAMPLES = 6;         // 60 ms of free fall
const unsigned long IMPACT_WINDOW_MS = 300;     // impact must follow within 300 ms
const float DROP_IMPACT_G = 2.0;
const float SHOCK_G       = 3.0;
const unsigned long SHOCK_COOLDOWN_MS = 2000;
const float FLIP_Z_G      = -0.5;               // Z axis pointing down
const float UPRIGHT_Z_G   = 0.5;
const uint8_t FLIP_SAMPLES = 100;               // 1 s

// Badge IDs (read with firmware/rfid_test)
const byte AUTH_UID[4]      = {0x40, 0x8C, 0x7D, 0xA2};   // white card
const byte INSPECTOR_UID[4] = {0x05, 0x24, 0x8E, 0xB9};   // blue key fob

// ------------------------------------------------------------------ hardware
const uint8_t LIGHT_PIN = A0;
const uint8_t RFID_SS = 10, RFID_RST = 9;
LiquidCrystal lcd(4, 5, 6, 7, 8, A1);
MFRC522 rfid(RFID_SS, RFID_RST);

uint8_t mpuAddr = 0x69;                         // 0x69 with AD0 tied to 3.3V
const uint8_t RTC_ADDR = 0x68;                  // DS3231
const uint8_t MEM_ADDR = 0x57;                  // AT24C32 on the ZS-042 clock module

const float ACCEL_LSB_PER_G  = 4096.0;          // +/-8 g
const float GYRO_LSB_PER_DPS = 65.5;            // +/-500 deg/s
float gyroOffset[3] = {0, 0, 0};

// Calendar date/time. Declared here, before any function: the Arduino IDE writes
// function prototypes above the first function, so custom types must come first.
struct DateTime { uint16_t y; uint8_t mo, d, h, mi, s; };

// ------------------------------------------------------------------ events
enum EventType : uint8_t {
  EV_BOOT = 1, EV_OPEN_OK, EV_OPEN_ALERT, EV_CLOSED, EV_DROP, EV_SHOCK,
  EV_FLIPPED, EV_UPRIGHT, EV_AUTH, EV_INSPECT, EV_BADGE_UNKNOWN, EV_CLEARED,
  EV_COUNT
};
// Short names (max 8 characters so they fit on the LCD next to the time)
const char N0[] PROGMEM = "?";        const char N1[] PROGMEM = "BOOT";
const char N2[] PROGMEM = "OPEN OK";  const char N3[] PROGMEM = "OPEN !!";
const char N4[] PROGMEM = "CLOSED";   const char N5[] PROGMEM = "DROP";
const char N6[] PROGMEM = "SHOCK";    const char N7[] PROGMEM = "FLIPPED";
const char N8[] PROGMEM = "UPRIGHT";  const char N9[] PROGMEM = "AUTH";
const char N10[] PROGMEM = "INSPECT"; const char N11[] PROGMEM = "BADGE ?";
const char N12[] PROGMEM = "CLEARED";
const char* const EVENT_NAMES[] PROGMEM = {N0, N1, N2, N3, N4, N5, N6, N7, N8, N9, N10, N11, N12};

bool isAlert(uint8_t t) {
  return t == EV_OPEN_ALERT || t == EV_DROP || t == EV_SHOCK || t == EV_FLIPPED || t == EV_BADGE_UNKNOWN;
}

void printEventName(Print &out, uint8_t t) {
  if (t >= EV_COUNT) t = 0;
  char buf[9];
  strncpy_P(buf, (PGM_P)pgm_read_word(&EVENT_NAMES[t]), 8);
  buf[8] = 0;
  out.print(buf);
}

// ------------------------------------------------------------------ small helpers
void writeReg(uint8_t dev, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(dev);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

uint8_t readReg(uint8_t dev, uint8_t reg) {
  Wire.beginTransmission(dev);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(dev, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}

void print2(Print &out, uint8_t v) {          // always two digits: 7 -> "07"
  if (v < 10) out.print('0');
  out.print(v);
}

// ------------------------------------------------------------------ time (DS3231)
// We keep time as "seconds since 1 Jan 1970" (Unix time): one number, easy to store.

uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

// Days since 1970-01-01 for a calendar date (H. Hinnant's algorithm)
long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned long doe = (unsigned long)yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + (long)doe - 719468L;
}

uint32_t toUnix(const DateTime &t) {
  return (uint32_t)daysFromCivil(t.y, t.mo, t.d) * 86400UL + t.h * 3600UL + t.mi * 60UL + t.s;
}

DateTime fromUnix(uint32_t u) {
  DateTime t;
  long z = u / 86400UL + 719468L;
  uint32_t secs = u % 86400UL;
  long era = z / 146097L;
  unsigned long doe = (unsigned long)(z - era * 146097L);
  unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long y = (long)yoe + era * 400;
  unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned long mp = (5 * doy + 2) / 153;
  t.d = doy - (153 * mp + 2) / 5 + 1;
  t.mo = mp < 10 ? mp + 3 : mp - 9;
  t.y = y + (t.mo <= 2);
  t.h = secs / 3600; t.mi = (secs / 60) % 60; t.s = secs % 60;
  return t;
}

bool rtcPresent = false;
uint32_t rtcUnix = 0;            // last time read from the clock
unsigned long rtcMillis = 0;     // millis() at that moment

bool rtcRead(DateTime &t) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(RTC_ADDR, (uint8_t)7) != 7) return false;
  t.s = bcd2bin(Wire.read() & 0x7F);
  t.mi = bcd2bin(Wire.read());
  t.h = bcd2bin(Wire.read() & 0x3F);
  Wire.read();                                   // day of week (unused)
  t.d = bcd2bin(Wire.read());
  t.mo = bcd2bin(Wire.read() & 0x1F);
  t.y = 2000 + bcd2bin(Wire.read());
  return true;
}

void rtcWrite(const DateTime &t) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x00);
  Wire.write(bin2bcd(t.s)); Wire.write(bin2bcd(t.mi)); Wire.write(bin2bcd(t.h));
  Wire.write(1);
  Wire.write(bin2bcd(t.d)); Wire.write(bin2bcd(t.mo)); Wire.write(bin2bcd(t.y - 2000));
  Wire.endTransmission();
  uint8_t status = readReg(RTC_ADDR, 0x0F);
  writeReg(RTC_ADDR, 0x0F, status & 0x7F);       // clear "oscillator stopped" flag
}

void rtcSync() {
  DateTime t;
  if (rtcPresent && rtcRead(t)) { rtcUnix = toUnix(t); rtcMillis = millis(); }
}

uint32_t nowUnix() {                             // current time, between two clock reads
  return rtcUnix + (millis() - rtcMillis) / 1000UL;
}

uint8_t monthFromName(const char *m) {
  const char names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  for (uint8_t i = 0; i < 12; i++)
    if (strncmp(m, names + i * 3, 3) == 0) return i + 1;
  return 1;
}

void printDateTime(Print &out, uint32_t u) {
  DateTime t = fromUnix(u);
  out.print(t.y); out.print('-'); print2(out, t.mo); out.print('-'); print2(out, t.d);
  out.print(' ');
  print2(out, t.h); out.print(':'); print2(out, t.mi); out.print(':'); print2(out, t.s);
}

// ------------------------------------------------------------------ tamper-evident log (AT24C32)
// Layout: bytes 0-5 header ('W','D', count, last chain), records of 8 bytes from address 32.
// Record: unix time (4) | event type (1) | value (1) | chain (2)
// chain = CRC16(previous chain + the 6 bytes of this record): a fingerprint linking every record
// to all the ones before it, like the numbered, glued pages of a ship's logbook.
const uint16_t LOG_START = 32;
const uint16_t LOG_CAPACITY = (4096 - LOG_START) / 8;    // 508 records, oldest overwritten
uint16_t logCount = 0;           // events ever written (keeps counting after wrap-around)
uint16_t logChain = 0xFFFF;
uint16_t alertCount = 0;
bool memPresent = false;

uint16_t crc16(uint16_t crc, const uint8_t *data, uint8_t len) {   // CRC-16/CCITT
  while (len--) {
    crc ^= (uint16_t)(*data++) << 8;
    for (uint8_t i = 0; i < 8; i++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
  }
  return crc;
}

void memWaitReady() {            // the chip is busy ~5 ms after a write: poll until it answers
  for (uint8_t i = 0; i < 20; i++) {
    Wire.beginTransmission(MEM_ADDR);
    if (Wire.endTransmission() == 0) return;
    delayMicroseconds(500);
  }
}

void memWrite(uint16_t addr, const uint8_t *data, uint8_t len) {   // len <= 16, same 32-byte page
  memWaitReady();
  Wire.beginTransmission(MEM_ADDR);
  Wire.write(addr >> 8);
  Wire.write(addr & 0xFF);
  Wire.write(data, len);
  Wire.endTransmission();
}

void memRead(uint16_t addr, uint8_t *data, uint8_t len) {
  memWaitReady();
  Wire.beginTransmission(MEM_ADDR);
  Wire.write(addr >> 8);
  Wire.write(addr & 0xFF);
  Wire.endTransmission(false);
  Wire.requestFrom(MEM_ADDR, len);
  for (uint8_t i = 0; i < len; i++) data[i] = Wire.available() ? Wire.read() : 0xFF;
}

void saveHeader() {
  uint8_t h[6] = {'W', 'D', (uint8_t)(logCount & 0xFF), (uint8_t)(logCount >> 8),
                  (uint8_t)(logChain & 0xFF), (uint8_t)(logChain >> 8)};
  memWrite(0, h, 6);
}

void loadHeader() {
  uint8_t h[6];
  memRead(0, h, 6);
  if (h[0] == 'W' && h[1] == 'D') {
    logCount = h[2] | (h[3] << 8);
    logChain = h[4] | (h[5] << 8);
  } else {                                       // blank memory: start a fresh log
    logCount = 0;
    logChain = 0xFFFF;
    saveHeader();
  }
}

bool readRecord(uint16_t seq, uint32_t &u, uint8_t &type, uint8_t &value, uint16_t &chain) {
  if (seq >= logCount || logCount - seq > LOG_CAPACITY) return false;
  uint8_t r[8];
  memRead(LOG_START + (seq % LOG_CAPACITY) * 8, r, 8);
  u = (uint32_t)r[0] | ((uint32_t)r[1] << 8) | ((uint32_t)r[2] << 16) | ((uint32_t)r[3] << 24);
  type = r[4]; value = r[5]; chain = r[6] | (r[7] << 8);
  return true;
}

void countAlerts() {
  alertCount = 0;
  uint16_t first = logCount > LOG_CAPACITY ? logCount - LOG_CAPACITY : 0;
  for (uint16_t s = first; s < logCount; s++) {
    uint32_t u; uint8_t t, v; uint16_t c;
    if (readRecord(s, u, t, v, c) && isAlert(t)) alertCount++;
  }
}

// What the screen shows as "last event"
uint8_t lastEventType = 0;
uint8_t lastEventValue = 0;
uint32_t lastEventTime = 0;

void logEvent(uint8_t type, uint8_t value) {
  uint32_t u = nowUnix();
  uint8_t r[8] = {(uint8_t)u, (uint8_t)(u >> 8), (uint8_t)(u >> 16), (uint8_t)(u >> 24), type, value, 0, 0};
  logChain = crc16(logChain, r, 6);
  r[6] = logChain & 0xFF;
  r[7] = logChain >> 8;
  if (memPresent) {
    memWrite(LOG_START + (logCount % LOG_CAPACITY) * 8, r, 8);
    logCount++;
    saveHeader();
  } else {
    logCount++;
  }
  if (isAlert(type)) alertCount++;
  lastEventType = type; lastEventValue = value; lastEventTime = u;

  // Tell the laptop too (lines starting with '#' are ignored by record.py)
  Serial.print(F("#E "));
  printDateTime(Serial, u);
  Serial.print(' ');
  printEventName(Serial, type);
  if (type == EV_DROP || type == EV_SHOCK) { Serial.print(' '); Serial.print(value / 10.0, 1); Serial.print('g'); }
  if (isAlert(type)) Serial.print(F(" ALERT"));
  Serial.println();
}

void dumpLog() {
  Serial.println(F("#L,seq,unix,datetime,event,value,chain"));
  uint16_t first = logCount > LOG_CAPACITY ? logCount - LOG_CAPACITY : 0;
  for (uint16_t s = first; s < logCount; s++) {
    uint32_t u; uint8_t t, v; uint16_t c;
    if (!readRecord(s, u, t, v, c)) continue;
    Serial.print(F("#L,")); Serial.print(s); Serial.print(',');
    Serial.print(u); Serial.print(',');
    printDateTime(Serial, u); Serial.print(',');
    printEventName(Serial, t); Serial.print(',');
    Serial.print(v); Serial.print(',');
    Serial.println(c, HEX);
  }
  Serial.print(F("#L,END,")); Serial.print(logCount); Serial.print(',');
  Serial.println(logChain, HEX);
}

void clearLog() {
  logCount = 0;
  logChain = 0xFFFF;
  alertCount = 0;
  if (memPresent) saveHeader();
  logEvent(EV_CLEARED, 0);
}

// ------------------------------------------------------------------ motion sensor
bool mpuRead(int16_t out[6]) {
  Wire.beginTransmission(mpuAddr);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(mpuAddr, (uint8_t)14) != 14) return false;
  int16_t v[7];
  for (uint8_t i = 0; i < 7; i++) {
    uint8_t hi = Wire.read();
    uint8_t lo = Wire.read();
    v[i] = (int16_t)((hi << 8) | lo);
  }
  out[0] = v[0]; out[1] = v[1]; out[2] = v[2];        // accel (v[3] = temperature, skipped)
  out[3] = v[4]; out[4] = v[5]; out[5] = v[6];        // gyro
  return true;
}

bool mpuSetup() {
  Wire.beginTransmission(0x69);
  mpuAddr = (Wire.endTransmission() == 0) ? 0x69 : 0x68;
  if (mpuAddr == 0x68) return false;               // 0x68 is the clock: AD0 must be tied to 3.3V
  writeReg(mpuAddr, 0x6B, 0x01);                   // wake up
  writeReg(mpuAddr, 0x1A, 0x03);                   // low-pass filter ~44 Hz
  writeReg(mpuAddr, 0x19, 0x09);                   // 100 Hz
  writeReg(mpuAddr, 0x1B, 0x08);                   // gyro +/-500 deg/s
  writeReg(mpuAddr, 0x1C, 0x10);                   // accel +/-8 g
  delay(100);
  long sum[3] = {0, 0, 0};
  int16_t raw[6];
  int good = 0;
  for (int i = 0; i < 200; i++) {
    if (mpuRead(raw)) { sum[0] += raw[3]; sum[1] += raw[4]; sum[2] += raw[5]; good++; }
    delay(10);
  }
  if (good == 0) return false;
  for (uint8_t k = 0; k < 3; k++) gyroOffset[k] = (float)sum[k] / good;
  return true;
}

// ------------------------------------------------------------------ rules (the THINK layer, v0)
uint8_t freefallCount = 0;
bool impactArmed = false;
unsigned long impactDeadline = 0;
float impactPeak = 0;
unsigned long lastShock = 0;
uint8_t flipCount = 0, uprightCount = 0;
bool flipped = false;

uint8_t gToValue(float g) {                       // store g x 10 in one byte (max 25.5 g)
  float v = g * 10.0;
  return v > 255 ? 255 : (uint8_t)v;
}

void runMotionRules(float ax, float ay, float az) {
  float mag = sqrt(ax * ax + ay * ay + az * az);
  unsigned long now = millis();

  // DROP = a stretch of free fall, then a hard landing
  if (mag < FREEFALL_G) {
    if (freefallCount < 255) freefallCount++;
  } else {
    if (freefallCount >= FREEFALL_MIN_SAMPLES && !impactArmed) {
      impactArmed = true;
      impactDeadline = now + IMPACT_WINDOW_MS;
      impactPeak = 0;
    }
    freefallCount = 0;
  }
  if (impactArmed) {
    if (mag > impactPeak) impactPeak = mag;
    if ((long)(now - impactDeadline) >= 0) {
      impactArmed = false;
      if (impactPeak > DROP_IMPACT_G) logEvent(EV_DROP, gToValue(impactPeak));
      lastShock = now;                            // the landing is not also a "shock"
    }
  } else if (mag > SHOCK_G && now - lastShock > SHOCK_COOLDOWN_MS) {
    // SHOCK = violent hit without free fall before it
    lastShock = now;
    logEvent(EV_SHOCK, gToValue(mag));
  }

  // FLIPPED = upside down (Z axis pointing to the ground) for 1 second
  if (!flipped) {
    flipCount = (az < FLIP_Z_G) ? flipCount + 1 : 0;
    if (flipCount >= FLIP_SAMPLES) { flipped = true; flipCount = 0; logEvent(EV_FLIPPED, 0); }
  } else {
    uprightCount = (az > UPRIGHT_Z_G) ? uprightCount + 1 : 0;
    if (uprightCount >= FLIP_SAMPLES) { flipped = false; uprightCount = 0; logEvent(EV_UPRIGHT, 0); }
  }
}

// ------------------------------------------------------------------ lid + badges
bool lidOpen = false;
unsigned long authUntil = 0;
bool inspectorMode = false;
uint16_t inspectorIndex = 0;              // how many records back from the newest
unsigned long inspectorNext = 0;
const uint8_t INSPECTOR_MAX = 10;

bool authActive() { return authUntil != 0 && (long)(authUntil - millis()) > 0; }

void checkLid(int light) {
  if (!lidOpen && light > LIGHT_OPEN) {
    lidOpen = true;
    bool authorised = authActive();
    authUntil = 0;                                // a badge authorises ONE opening, then expires
    logEvent(authorised ? EV_OPEN_OK : EV_OPEN_ALERT, light >> 2);
  } else if (lidOpen && light < LIGHT_CLOSED) {
    lidOpen = false;
    logEvent(EV_CLOSED, light >> 2);
  }
}

bool sameUid(const byte *a) {
  if (rfid.uid.size != 4) return false;
  for (uint8_t i = 0; i < 4; i++) if (rfid.uid.uidByte[i] != a[i]) return false;
  return true;
}

void checkBadge() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;
  if (sameUid(AUTH_UID)) {
    authUntil = millis() + AUTH_WINDOW_MS;
    logEvent(EV_AUTH, 0);
  } else if (sameUid(INSPECTOR_UID)) {
    logEvent(EV_INSPECT, 0);
    inspectorMode = true;
    inspectorIndex = 0;
    inspectorNext = 0;
  } else {
    logEvent(EV_BADGE_UNKNOWN, rfid.uid.uidByte[0]);
  }
  rfid.PICC_HaltA();
}

// ------------------------------------------------------------------ screen
char shown[2][17];                        // what is currently on the LCD (avoid useless rewrites)

void lcdLine(uint8_t row, const char *text) {
  char buf[17];
  uint8_t i = 0;
  for (; i < 16 && text[i]; i++) buf[i] = text[i];
  for (; i < 16; i++) buf[i] = ' ';
  buf[16] = 0;
  if (strcmp(buf, shown[row]) == 0) return;
  strcpy(shown[row], buf);
  lcd.setCursor(0, row);
  lcd.print(buf);
}

// Tiny "string builder" so we can compose LCD lines without heavy libraries
struct Line : public Print {
  char s[17]; uint8_t n = 0;
  Line() { s[0] = 0; }
  size_t write(uint8_t c) override { if (n < 16) { s[n++] = c; s[n] = 0; } return 1; }
};

void updateScreen() {
  Line l1, l2;
  if (inspectorMode) {
    if ((long)(millis() - inspectorNext) < 0) return;
    inspectorNext = millis() + 2500;      // one record every 2.5 s, newest first
    uint16_t available = logCount > LOG_CAPACITY ? LOG_CAPACITY : logCount;
    uint16_t n = available < INSPECTOR_MAX ? available : INSPECTOR_MAX;
    if (inspectorIndex >= n) { inspectorMode = false; return; }
    uint32_t u; uint8_t t, v; uint16_t c;
    readRecord(logCount - 1 - inspectorIndex, u, t, v, c);
    DateTime d = fromUnix(u);
    l1.print(F("LOG ")); l1.print(inspectorIndex + 1); l1.print('/'); l1.print(n);
    l1.print(' '); printEventName(l1, t);
    print2(l2, d.d); l2.print('/'); print2(l2, d.mo); l2.print(' ');
    print2(l2, d.h); l2.print(':'); print2(l2, d.mi);
    if (t == EV_DROP || t == EV_SHOCK) { l2.print(' '); l2.print(v / 10.0, 1); l2.print('g'); }
    inspectorIndex++;
  } else {
    DateTime d = fromUnix(nowUnix());
    if (alertCount == 0) l1.print(F("SEALED  "));
    else { l1.print(F("ALERT ")); l1.print(alertCount > 9 ? 9 : alertCount); l1.print(' '); }
    print2(l1, d.h); l1.print(':'); print2(l1, d.mi); l1.print(':'); print2(l1, d.s);
    if (authActive()) {
      l2.print(F("AUTH OPEN  "));
      l2.print((authUntil - millis()) / 1000 + 1); l2.print('s');
    } else if (lastEventType) {
      DateTime e = fromUnix(lastEventTime);
      printEventName(l2, lastEventType);
      if (lastEventType == EV_DROP || lastEventType == EV_SHOCK) {   // e.g. "DROP 3.1g  16:42"
        l2.print(' '); l2.print(lastEventValue / 10.0, 1); l2.print('g');
      }
      while (l2.n < 11) l2.print(' ');
      print2(l2, e.h); l2.print(':'); print2(l2, e.mi);
    } else {
      l2.print(F("no event"));
    }
  }
  lcdLine(0, l1.s);
  lcdLine(1, l2.s);
}

// Status line for the phone app, every 2 s: #S,<alerts>,<lid 0|1>,<auth seconds left>,<unix time>
void sendStatus() {
  Serial.print(F("#S,"));
  Serial.print(alertCount); Serial.print(',');
  Serial.print(lidOpen ? 1 : 0); Serial.print(',');
  Serial.print(authActive() ? (authUntil - millis()) / 1000 + 1 : 0); Serial.print(',');
  Serial.println(nowUnix());
}

// ------------------------------------------------------------------ serial commands
char cmd[28];
uint8_t cmdLen = 0;

void handleCommand() {
  if (strcmp(cmd, "dump") == 0) dumpLog();
  else if (strcmp(cmd, "clear") == 0) clearLog();
  else if (strncmp(cmd, "time ", 5) == 0 && strlen(cmd) >= 24) {
    // time YYYY-MM-DD HH:MM:SS
    DateTime t;
    t.y = atoi(cmd + 5); t.mo = atoi(cmd + 10); t.d = atoi(cmd + 13);
    t.h = atoi(cmd + 16); t.mi = atoi(cmd + 19); t.s = atoi(cmd + 22);
    rtcWrite(t);
    rtcSync();
    Serial.print(F("# clock set to ")); printDateTime(Serial, nowUnix()); Serial.println();
  } else {
    Serial.println(F("# commands: dump | clear | time YYYY-MM-DD HH:MM:SS"));
  }
}

void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdLen) { cmd[cmdLen] = 0; handleCommand(); cmdLen = 0; }
    } else if (cmdLen < sizeof(cmd) - 1) {
      cmd[cmdLen++] = c;
    }
  }
}

// ------------------------------------------------------------------ setup & loop
unsigned long nextSampleUs = 0;
unsigned long nextBadgeCheck = 0, nextScreen = 0, nextRtcSync = 0, nextStatus = 0;

void halt(const __FlashStringHelper *line1, const __FlashStringHelper *msg) {
  lcd.clear(); lcd.print(line1);
  lcd.setCursor(0, 1); lcd.print(F("see laptop"));
  while (true) { Serial.print(F("# ERROR: ")); Serial.println(msg); delay(2000); }
}

void setup() {
  Serial.begin(115200);
  lcd.begin(16, 2);
  lcd.print(F("WARDEN v0.3"));
  lcd.setCursor(0, 1);
  lcd.print(F("starting..."));
  Serial.println(F("# WARDEN firmware v0.3"));

  Wire.begin();
  Wire.setClock(400000);

  // Clock: if it lost its time (or is brand new), start from the moment this code was compiled.
  Wire.beginTransmission(RTC_ADDR);
  rtcPresent = (Wire.endTransmission() == 0);
  if (rtcPresent) {
    if (readReg(RTC_ADDR, 0x0F) & 0x80) {
      DateTime t;
      const char *date = __DATE__;              // "Oct 21 2026"
      const char *tim = __TIME__;               // "16:30:12"
      t.mo = monthFromName(date); t.d = atoi(date + 4); t.y = atoi(date + 7);
      t.h = atoi(tim); t.mi = atoi(tim + 3); t.s = atoi(tim + 6);
      rtcWrite(t);
      Serial.println(F("# clock had lost its time: set to compile time"));
    }
    rtcSync();
  } else {
    Serial.println(F("# WARNING: DS3231 clock not found, times start at 1970"));
  }

  // Memory for the log
  Wire.beginTransmission(MEM_ADDR);
  memPresent = (Wire.endTransmission() == 0);
  if (memPresent) { loadHeader(); countAlerts(); }
  else Serial.println(F("# WARNING: log memory (0x57) not found, events will not be saved"));

  // RFID reader, with a shorter radio timeout (5 ms instead of 25 ms) so polling
  // for badges does not make us miss motion samples.
  SPI.begin();
  rfid.PCD_Init();
  rfid.PCD_WriteRegister(MFRC522::TReloadRegH, 0x00);
  rfid.PCD_WriteRegister(MFRC522::TReloadRegL, 0xC8);
  byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  if (v == 0x00 || v == 0xFF) Serial.println(F("# WARNING: RFID reader not responding"));

  // Motion sensor (keep the crate still: 2 s gyro calibration)
  lcd.setCursor(0, 1);
  lcd.print(F("keep still...   "));
  Serial.println(F("# Keep the sensor perfectly still: calibrating gyro (2 s)..."));
  if (!mpuSetup()) halt(F("SENSOR ERROR"), F("motion sensor not found at 0x69 (check AD0 -> 3.3V)"));
  Serial.println(F("# Calibration done."));

  // Initial lid state without raising an alarm
  lidOpen = analogRead(LIGHT_PIN) > LIGHT_OPEN;

  Serial.print(F("# log: ")); Serial.print(logCount); Serial.print(F(" events, "));
  Serial.print(alertCount); Serial.println(F(" alerts"));
  logEvent(EV_BOOT, 0);

  lcd.clear();
  shown[0][0] = shown[1][0] = 0;
  Serial.println(F("t_ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,light"));
  nextSampleUs = micros();
}

void loop() {
  readSerial();

  if ((long)(micros() - nextSampleUs) < 0) return;      // wait for the next 10 ms tick
  nextSampleUs += SAMPLE_PERIOD_US;

  int16_t raw[6];
  if (!mpuRead(raw)) { Serial.println(F("# read error")); return; }
  int light = analogRead(LIGHT_PIN);
  float ax = raw[0] / ACCEL_LSB_PER_G, ay = raw[1] / ACCEL_LSB_PER_G, az = raw[2] / ACCEL_LSB_PER_G;

  // 1. stream the sample (same format as imu_stream, so record.py keeps working)
  Serial.print(millis());                                          Serial.print(',');
  Serial.print(ax, 3);                                             Serial.print(',');
  Serial.print(ay, 3);                                             Serial.print(',');
  Serial.print(az, 3);                                             Serial.print(',');
  Serial.print((raw[3] - gyroOffset[0]) / GYRO_LSB_PER_DPS, 2);   Serial.print(',');
  Serial.print((raw[4] - gyroOffset[1]) / GYRO_LSB_PER_DPS, 2);   Serial.print(',');
  Serial.print((raw[5] - gyroOffset[2]) / GYRO_LSB_PER_DPS, 2);   Serial.print(',');
  Serial.println(light);

  // 2. rules, lid, badges, screen, clock (the slower jobs run only every few ticks)
  runMotionRules(ax, ay, az);
  checkLid(light);
  unsigned long now = millis();
  if ((long)(now - nextBadgeCheck) >= 0) { nextBadgeCheck = now + 300; checkBadge(); }
  else if ((long)(now - nextScreen) >= 0) { nextScreen = now + 250; updateScreen(); }
  else if ((long)(now - nextRtcSync) >= 0) { nextRtcSync = now + 10000; rtcSync(); }
  else if ((long)(now - nextStatus) >= 0) { nextStatus = now + 2000; sendStatus(); }
}
