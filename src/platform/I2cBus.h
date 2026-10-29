#pragma once
// Shared I2C bus with mutex, transaction timeout and bus recovery.
// The OLED shares this bus with the output expanders: every transfer is
// short and bounded so a defective display cannot hold the bus.
#include <Arduino.h>
#include <Wire.h>

#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace platform {

class I2cBus {
 public:
  bool begin(int sda, int scl, uint32_t hz);
  // Lock with timeout; returns false if the bus could not be acquired.
  bool lock(uint32_t timeoutMs = 50);
  void unlock();

  bool probe(uint8_t addr);
  bool write(uint8_t addr, const uint8_t* data, size_t len);
  bool writeByte(uint8_t addr, uint8_t v) { return write(addr, &v, 1); }
  bool read(uint8_t addr, uint8_t* data, size_t len);
  bool writeRead(uint8_t addr, const uint8_t* w, size_t wl, uint8_t* r, size_t rl);
  std::vector<uint8_t> scan();
  // Clocks SCL until SDA is released, then re-initialises the driver.
  void recover();

  uint32_t errors() const { return errors_; }
  uint32_t recoveries() const { return recoveries_; }

 private:
  void noteResult(bool ok);
  SemaphoreHandle_t mtx_ = nullptr;
  int sda_ = -1, scl_ = -1;
  uint32_t hz_ = 100000;
  uint32_t errors_ = 0, recoveries_ = 0;
  uint8_t consecutive_ = 0;
};

// RAII helper
class I2cLock {
 public:
  explicit I2cLock(I2cBus& b, uint32_t timeoutMs = 50) : b_(b), ok_(b.lock(timeoutMs)) {}
  ~I2cLock() {
    if (ok_) b_.unlock();
  }
  bool ok() const { return ok_; }

 private:
  I2cBus& b_;
  bool ok_;
};

}  // namespace platform
