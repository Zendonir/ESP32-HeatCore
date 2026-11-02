#include "platform/I2cBus.h"

namespace platform {

bool I2cBus::begin(int sda, int scl, uint32_t hz) {
  sda_ = sda;
  scl_ = scl;
  hz_ = hz;
  if (!mtx_) mtx_ = xSemaphoreCreateMutex();
  recover();  // release a bus that may be stuck after a reset
  bool ok = Wire.begin(sda, scl, hz);
  Wire.setTimeOut(20);
  return ok;
}

bool I2cBus::lock(uint32_t timeoutMs) { return mtx_ && xSemaphoreTake(mtx_, pdMS_TO_TICKS(timeoutMs)) == pdTRUE; }

void I2cBus::unlock() {
  if (mtx_) xSemaphoreGive(mtx_);
}

void I2cBus::noteResult(bool ok) {
  if (ok) {
    consecutive_ = 0;
    return;
  }
  ++errors_;
  if (++consecutive_ >= 5) {
    consecutive_ = 0;
    recover();
  }
}

bool I2cBus::probe(uint8_t addr) {
  // A one-byte read instead of an empty write: Arduino-ESP32 3.x maps the
  // empty write to i2c_master_probe(), which on the KC868-A16 V3 reported
  // ghost devices and missed the DS3231/24C02 (field self-test). Reading one
  // byte is harmless for every device on this bus (PCF8574 port, DS3231 and
  // 24C02 current address, SSD1306 status).
  size_t n = Wire.requestFrom(addr, static_cast<size_t>(1));
  while (Wire.available()) Wire.read();
  return n == 1;
}

bool I2cBus::write(uint8_t addr, const uint8_t* data, size_t len) {
  Wire.beginTransmission(addr);
  Wire.write(data, len);
  bool ok = Wire.endTransmission() == 0;
  noteResult(ok);
  return ok;
}

bool I2cBus::read(uint8_t addr, uint8_t* data, size_t len) {
  size_t n = Wire.requestFrom(addr, len);
  bool ok = n == len;
  for (size_t i = 0; i < n && i < len; ++i) data[i] = static_cast<uint8_t>(Wire.read());
  noteResult(ok);
  return ok;
}

bool I2cBus::writeRead(uint8_t addr, const uint8_t* w, size_t wl, uint8_t* r, size_t rl) {
  Wire.beginTransmission(addr);
  Wire.write(w, wl);
  if (Wire.endTransmission(false) != 0) {
    noteResult(false);
    return false;
  }
  return read(addr, r, rl);
}

std::vector<uint8_t> I2cBus::scan() {
  // Lock per address: an absent address may take until the transaction
  // timeout, and holding the bus for the whole scan (> 1 s) starved the IO
  // task (input read / output write failures, output watchdog alarm).
  std::vector<uint8_t> found;
  for (uint8_t a = 0x08; a < 0x78; ++a) {
    I2cLock lk(*this, 200);
    if (lk.ok() && probe(a)) found.push_back(a);
  }
  return found;
}

void I2cBus::recover() {
  ++recoveries_;
  Wire.end();
  pinMode(sda_, INPUT_PULLUP);
  pinMode(scl_, OUTPUT_OPEN_DRAIN);
  for (int i = 0; i < 9 && digitalRead(sda_) == LOW; ++i) {
    digitalWrite(scl_, LOW);
    delayMicroseconds(5);
    digitalWrite(scl_, HIGH);
    delayMicroseconds(5);
  }
  // STOP condition
  pinMode(sda_, OUTPUT_OPEN_DRAIN);
  digitalWrite(sda_, LOW);
  delayMicroseconds(5);
  digitalWrite(scl_, HIGH);
  delayMicroseconds(5);
  digitalWrite(sda_, HIGH);
  delayMicroseconds(5);
  Wire.begin(sda_, scl_, hz_);
  Wire.setTimeOut(20);
}

}  // namespace platform
