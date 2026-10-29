#pragma once
// Hardware drivers of the KC868-A16 V3 used by HeatCore.
#include <Arduino.h>

#include <array>
#include <string>
#include <vector>

#include "hc/display/Framebuffer.h"
#include "hc/rf/RfCodec.h"
#include "hc/rf/RfTransmitter.h"
#include "platform/I2cBus.h"

class OneWire;
class DallasTemperature;

namespace platform {

// ------------------------------------------------------------- PCF8574 I/O
class IoExpanders {
 public:
  explicit IoExpanders(I2cBus& bus) : bus_(bus) {}
  // Writes the safe output state immediately (first action after reset).
  bool begin(uint16_t safeMask);
  bool writeOutputs(uint16_t logicalMask, bool verify);
  bool readInputs(uint16_t& activeMask);
  bool outputsPresent() const { return out1_ && out2_; }
  bool inputsPresent() const { return in1_ && in2_; }
  uint16_t lastWritten() const { return last_; }

 private:
  bool writePort(uint8_t addr, uint8_t logical8);
  I2cBus& bus_;
  bool out1_ = false, out2_ = false, in1_ = false, in2_ = false;
  uint16_t last_ = 0;
};

// ------------------------------------------------------------------ DS3231
class Ds3231 {
 public:
  explicit Ds3231(I2cBus& bus) : bus_(bus) {}
  bool begin();
  bool present() const { return present_; }
  // Returns false when the oscillator-stop flag says the time is invalid.
  bool read(time_t& utc);
  bool write(time_t utc);
  bool temperature(float& c);

 private:
  I2cBus& bus_;
  bool present_ = false;
};

// ----------------------------------------------------------------- SSD1306
class Ssd1306 {
 public:
  explicit Ssd1306(I2cBus& bus, uint8_t addr) : bus_(bus), addr_(addr) {}
  bool init(uint8_t contrast, bool rotate180);
  bool flush(const hc::Framebuffer& fb, bool force);  // sends changed pages only
  bool setContrast(uint8_t c);
  bool setRotation(bool rotate180);
  bool power(bool on);
  bool initialized() const { return init_; }
  uint32_t lastFlushUs() const { return lastFlushUs_; }
  void invalidate() { haveShadow_ = false; }

 private:
  bool cmd(std::initializer_list<uint8_t> c);
  I2cBus& bus_;
  uint8_t addr_;
  bool init_ = false;
  bool haveShadow_ = false;
  bool on_ = true;
  uint8_t shadow_[hc::Framebuffer::kBytes] = {0};
  uint32_t lastFlushUs_ = 0;
};

// ------------------------------------------------------------- RF433 RX/TX
namespace rfhw {
bool beginRx(int pin);
// Copies pending pulses (ISR ring buffer) into out. Returns count.
size_t drain(hc::rf::Pulse* out, size_t max);
uint32_t edges();
uint32_t dropped();
uint32_t usSinceLastEdge();
}  // namespace rfhw

class RmtTxBackend : public hc::rf::ITxBackend {
 public:
  bool begin(int pin);
  bool ready() const override { return ready_; }
  bool transmit(const std::vector<hc::rf::Pulse>& pulses, uint32_t timeoutMs) override;

 private:
  int pin_ = -1;
  bool ready_ = false;
};

// --------------------------------------------------------------- 1-Wire
class OneWireBuses {
 public:
  struct Reading {
    std::string id;
    float value;
    bool ok;
  };
  void begin(const int* pins, size_t n);
  // Starts conversion on all buses (non-blocking).
  void request();
  // Reads results (call >= 800 ms after request()).
  void collect(std::vector<Reading>& out);
  void rescan();
  uint8_t count(size_t bus) const { return bus < counts_.size() ? counts_[bus] : 0; }

 private:
  std::vector<OneWire*> wires_;
  std::vector<DallasTemperature*> dallas_;
  std::vector<std::vector<std::array<uint8_t, 8>>> addrs_;
  std::array<uint8_t, 3> counts_{{0, 0, 0}};
};

// ------------------------------------------------------------- RS485 lines
class Rs485Lines {
 public:
  void begin(int rx, int tx, uint32_t baud);
  // Returns true and fills `line` when a complete line was received.
  bool poll(std::string& line);
  bool active() const { return active_; }

 private:
  std::string buf_;
  bool active_ = false;
  bool overflow_ = false;
};

}  // namespace platform
