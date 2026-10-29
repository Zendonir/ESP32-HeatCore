#pragma once
// Plug-in interface for RF433 temperature sensor decoders.
//
// "433 MHz" does not imply compatibility. Only explicitly implemented
// protocols are decoded:
//   * nexus_th  - Nexus/"TH" family (36 bit PPM, used by several cheap weather
//                 station sensors). Implemented from the public rtl_433
//                 protocol description, verified with synthetic test vectors
//                 only -> marked hardwareVerified() == false and disabled by
//                 default until a real sensor has been confirmed.
//   * rtl433    - JSON lines from an external receiver running rtl_433
//                 (e.g. via RS485/UART gateway). This is the recommended path
//                 for sensors the on-board receiver cannot decode.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hc/core/Clock.h"
#include "hc/rf/RfCodec.h"

namespace hc {
namespace rf {

struct SensorReading {
  std::string decoder;  // "nexus_th", "rtl433:<model>"
  uint32_t id = 0;
  uint8_t channel = 0;
  bool hasTemperature = false;
  float temperatureC = 0.f;
  bool hasHumidity = false;
  float humidity = 0.f;
  int8_t batteryOk = -1;  // -1 unknown
  uint8_t quality = 0;
  // Sensor key used by the sensor manager, e.g. "rf:nexus_th:12:1"
  std::string key() const;
};

class IRfSensorDecoder {
 public:
  virtual ~IRfSensorDecoder() = default;
  virtual const char* name() const = 0;
  virtual const char* description() const = 0;
  virtual bool hardwareVerified() const = 0;
  // Decode one segment (row) of pulses. Return true and fill `out` and
  // `rowBits`/`rowLen` (raw row used for repeat confirmation) on success.
  virtual bool decodeRow(const Pulse* p, size_t n, SensorReading& out, uint64_t& rowBits, uint8_t& rowLen) = 0;
};

class NexusThDecoder : public IRfSensorDecoder {
 public:
  const char* name() const override { return "nexus_th"; }
  const char* description() const override {
    return "Nexus-TH 36 bit PPM (Temperatur/Feuchte), nach rtl_433-Protokollbeschreibung";
  }
  bool hardwareVerified() const override { return false; }
  bool decodeRow(const Pulse* p, size_t n, SensorReading& out, uint64_t& rowBits, uint8_t& rowLen) override;
  // Builds a row as transmitted (for tests / simulation).
  static std::vector<Pulse> encodeRow(uint8_t id, bool batteryOk, uint8_t channel, float tempC, uint8_t humidity);
};

class RfSensorHub {
 public:
  using Listener = void (*)(void* ctx, const SensorReading& r);
  struct DecoderSlot {
    IRfSensorDecoder* decoder;
    bool enabled;
    uint32_t rows = 0;
    uint32_t accepted = 0;
  };

  void addDecoder(IRfSensorDecoder* d, bool enabled);
  bool setEnabled(const std::string& name, bool enabled);
  bool isEnabled(const std::string& name) const;
  const std::vector<DecoderSlot>& decoders() const { return slots_; }
  void setListener(Listener l, void* ctx) { listener_ = l; ctx_ = ctx; }

  // Returns true if any decoder consumed the segment.
  bool onSegment(const Pulse* p, size_t n, Millis now);
  // Readings from external sources (rtl_433 JSON). Already confirmed upstream.
  void injectExternal(const SensorReading& r, Millis now);

  uint8_t minRows = 2;          // identical rows required within confirmWindowMs
  uint16_t confirmWindowMs = 1500;
  uint16_t dedupeMs = 4000;     // suppress repeated identical readings

 private:
  struct Recent {
    uint64_t bits;
    uint8_t len;
    uint8_t count;
    Millis first;
    Millis lastDelivered;
    bool delivered;
  };
  std::vector<DecoderSlot> slots_;
  std::vector<Recent> recent_;
  Listener listener_ = nullptr;
  void* ctx_ = nullptr;
};

// Parses one rtl_433 JSON line (`rtl_433 -F json`). Requires "model" and "id".
bool parseRtl433Json(const char* line, SensorReading& out, std::string& err);

}  // namespace rf
}  // namespace hc
