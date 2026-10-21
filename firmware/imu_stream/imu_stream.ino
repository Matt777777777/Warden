/*
  WARDEN - Week 1 firmware: stream motion + light data to the laptop
  ------------------------------------------------------------------
  What it does
    Reads the GY-521 (MPU-6050) accelerometer/gyroscope and a light sensor
    100 times per second and prints one CSV line per sample over USB:

        t_ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,light

    ax/ay/az  acceleration in g (1 g = gravity). At rest, one axis reads ~1.
    gx/gy/gz  rotation speed in degrees per second. At rest, all ~0.
    light     0 (dark) to 1023 (bright). Used later to detect the lid opening.

    Lines starting with '#' are messages for humans; the Python tools skip them.

  Wiring (see docs/wiring_week1.png)
    GY-521 VCC -> 5V      GY-521 GND -> GND
    GY-521 SDA -> A4      GY-521 SCL -> A5      GY-521 AD0 -> 3.3V (address 0x69)
    Photoresistor: 5V -> photoresistor -> A0 -> 10k resistor -> GND

  No extra library needed: we talk to the sensor directly with Wire (I2C).
*/

#include <Wire.h>

// ---------- Settings you may want to change ----------
uint8_t MPU_ADDR = 0x69;                     // 0x69 when AD0 is tied to 3.3V (frees 0x68 for the DS3231 clock)
const uint8_t LIGHT_PIN = A0;                // photoresistor divider output
const unsigned long SAMPLE_PERIOD_US = 10000; // 10 000 us = 100 samples per second

// ---------- Sensor scale factors ----------
// We pick +/-8 g because a drop can easily exceed 2 g, and +/-500 deg/s for rotations.
// The sensor returns integers; dividing by these numbers gives real units.
const float ACCEL_LSB_PER_G   = 4096.0;  // datasheet value for +/-8 g
const float GYRO_LSB_PER_DPS  = 65.5;    // datasheet value for +/-500 deg/s

// MPU-6050 register addresses (from the datasheet)
const uint8_t REG_SMPLRT_DIV   = 0x19;
const uint8_t REG_CONFIG       = 0x1A;
const uint8_t REG_GYRO_CONFIG  = 0x1B;
const uint8_t REG_ACCEL_CONFIG = 0x1C;
const uint8_t REG_ACCEL_XOUT_H = 0x3B;
const uint8_t REG_PWR_MGMT_1   = 0x6B;
const uint8_t REG_WHO_AM_I     = 0x75;

float gyroOffset[3] = {0, 0, 0};   // measured at startup, subtracted from every reading
unsigned long nextSampleUs = 0;

// Write one byte into one register of the sensor
void writeRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

// Read a 16-bit signed number sent as two bytes (high byte first)
int16_t readInt16() {
  uint8_t high = Wire.read();
  uint8_t low  = Wire.read();
  return (int16_t)((high << 8) | low);
}

// Read accel X,Y,Z then gyro X,Y,Z as raw integers. Returns false if the sensor did not answer.
bool readRaw(int16_t out[6]) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(REG_ACCEL_XOUT_H);              // start reading at ACCEL_XOUT_H...
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MPU_ADDR, (uint8_t)14) != 14) return false;  // ...14 bytes in a row

  out[0] = readInt16();   // accel X
  out[1] = readInt16();   // accel Y
  out[2] = readInt16();   // accel Z
  readInt16();            // temperature (not used)
  out[3] = readInt16();   // gyro X
  out[4] = readInt16();   // gyro Y
  out[5] = readInt16();   // gyro Z
  return true;
}

// Stop here forever and keep repeating an error message
void halt(const __FlashStringHelper *message) {
  while (true) {
    Serial.print(F("# ERROR: "));
    Serial.println(message);
    delay(2000);
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  Wire.setClock(400000);  // fast I2C (400 kHz) so reading takes well under 1 ms

  Serial.println(F("# WARDEN imu_stream v0.1"));

  // 1. Is anybody there? The sensor answers at 0x69 (AD0 tied to 3.3V) or 0x68 (AD0 free).
  Wire.beginTransmission(0x69);
  if (Wire.endTransmission() != 0) MPU_ADDR = 0x68;
  Serial.print(F("# MPU-6050 at 0x"));
  Serial.println(MPU_ADDR, HEX);
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(REG_WHO_AM_I);
  if (Wire.endTransmission(false) != 0) {
    halt(F("MPU-6050 not responding. Check SDA->A4, SCL->A5, VCC->5V, GND->GND."));
  }
  Wire.requestFrom(MPU_ADDR, (uint8_t)1);
  uint8_t id = Wire.read();
  if (id != 0x68) {
    // Some clone chips report another ID but work the same way.
    Serial.print(F("# WARNING: unexpected sensor ID 0x"));
    Serial.print(id, HEX);
    Serial.println(F(" (clone chip?). Continuing."));
  }

  // 2. Configure the sensor
  writeRegister(REG_PWR_MGMT_1, 0x01);    // wake up (it starts asleep), use gyro clock
  writeRegister(REG_CONFIG, 0x03);        // built-in low-pass filter ~44 Hz (removes jitter)
  writeRegister(REG_SMPLRT_DIV, 0x09);    // internal rate 1 kHz / (1+9) = 100 Hz
  writeRegister(REG_GYRO_CONFIG, 0x08);   // gyro range +/-500 deg/s
  writeRegister(REG_ACCEL_CONFIG, 0x10);  // accel range +/-8 g
  delay(100);

  // 3. Calibrate the gyro: at rest it should read 0, but every chip has a small bias.
  Serial.println(F("# Keep the sensor perfectly still: calibrating gyro (2 s)..."));
  const int N = 200;
  long sum[3] = {0, 0, 0};
  int16_t raw[6];
  int good = 0;
  for (int i = 0; i < N; i++) {
    if (readRaw(raw)) {
      sum[0] += raw[3];
      sum[1] += raw[4];
      sum[2] += raw[5];
      good++;
    }
    delay(10);
  }
  if (good == 0) halt(F("Could not read the sensor during calibration."));
  for (int k = 0; k < 3; k++) gyroOffset[k] = (float)sum[k] / good;
  Serial.println(F("# Calibration done."));

  // 4. Column names, then data starts
  Serial.println(F("t_ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,light"));
  nextSampleUs = micros();
}

void loop() {
  // Wait for the next 10 ms tick. The subtraction trick stays correct when micros() wraps around.
  if ((long)(micros() - nextSampleUs) < 0) return;
  nextSampleUs += SAMPLE_PERIOD_US;

  int16_t raw[6];
  if (!readRaw(raw)) {
    Serial.println(F("# read error"));
    return;
  }
  int light = analogRead(LIGHT_PIN);

  Serial.print(millis());                                         Serial.print(',');
  Serial.print(raw[0] / ACCEL_LSB_PER_G, 3);                      Serial.print(',');
  Serial.print(raw[1] / ACCEL_LSB_PER_G, 3);                      Serial.print(',');
  Serial.print(raw[2] / ACCEL_LSB_PER_G, 3);                      Serial.print(',');
  Serial.print((raw[3] - gyroOffset[0]) / GYRO_LSB_PER_DPS, 2);   Serial.print(',');
  Serial.print((raw[4] - gyroOffset[1]) / GYRO_LSB_PER_DPS, 2);   Serial.print(',');
  Serial.print((raw[5] - gyroOffset[2]) / GYRO_LSB_PER_DPS, 2);   Serial.print(',');
  Serial.println(light);
}
