#pragma once
// Central sensor manager. All temperature sources (1-Wire, RF433 decoders,
// external rtl_433 gateway, analog inputs, Bluetooth thermometers) are handled identically:
// offset correction, plausibility, timeout, priority and fallback sensor.
#include <ArduinoJson.h>

#include <cstdint>
#include <string>
#include <vector>

#include "hc/core/Clock.h"
#include "hc/core/Event.h"

namespace hc {

enum class SensorKind : uint8_t { OneWire = 0, Rf, External, Analog, Virtual, Ble };
enum class SensorRole : uint8_t { Room = 0, Flow, Return, Outdoor, Other };
enum class SensorStatus : uint8_t { Never = 0, Ok, Stale, Fault, Disabled };

const char* sensorKindName(SensorKind k);
SensorKind sensorKindFromKey(const std::string& id);
const char* sensorRoleName(SensorRole r);
SensorRole sensorRoleFromName(const char* s);
const char* sensorStatusName(SensorStatus s);

struct Sensor {
  // configuration
  std::string id;    // "ow1:28FF0A1B2C3D4E5F", "rf:nexus_th:12:1", "ai:2", "ble:A4C1381A2B3C", ...
  std::string name;
  SensorKind kind = SensorKind::OneWire;
  SensorRole role = SensorRole::Room;
  std::string room;
  float offset = 0.f;
  uint32_t timeoutSec = 300;
  int priority = 50;          // higher wins within a room
  std::string fallbackId;     // used when this sensor is not available
  bool enabled = true;
  float minValid = -40.f, maxValid = 100.f;
  // runtime
  SensorStatus status = SensorStatus::Never;
  float raw = 0.f;
  float value = 0.f;
  bool hasHumidity = false;
  float humidity = 0.f;
  int8_t batteryOk = -1;
  int8_t batteryPct = -1;  // battery level in percent, -1 unknown
  uint8_t quality = 0;
  Millis lastUpdateMs = 0;
  Millis configuredMs = 0;  // when the sensor was added/loaded (startup supervision)
  int64_t lastEpoch = 0;
  uint32_t updates = 0;
  uint32_t errors = 0;
  uint32_t timeouts = 0;
  std::string lastError;

  bool available() const { return status == SensorStatus::Ok; }
};

// Found by a bus scan or RF decoder but not yet adopted by the user.
struct DiscoveredSensor {
  std::string id;
  SensorKind kind;
  float lastValue = 0.f;
  bool hasHumidity = false;
  float humidity = 0.f;
  int8_t batteryOk = -1;
  int8_t batteryPct = -1;
  uint32_t seen = 0;
  Millis lastMs = 0;
};

struct RoomTemperature {
  bool valid = false;
  float value = 0.f;
  std::string sensorId;
  bool fallback = false;  // provided by the fallback chain
};

class SensorManager {
 public:
  static constexpr size_t kMaxSensors = 64;
  static constexpr size_t kMaxDiscovered = 24;

  SensorManager(EventBus& bus, const IClock& clock) : bus_(bus), clock_(clock) {}

  // Value delivery from drivers/decoders. Unknown ids go to the discovery
  // list (autoAdopt false) or are adopted automatically.
  void update(const std::string& id, float raw, Millis now);
  void updateExtended(const std::string& id, float raw, bool hasHum, float hum, int8_t battery, uint8_t quality,
                      Millis now, int8_t batteryPct = -1);
  void reportError(const std::string& id, const char* err, Millis now);
  void tick(Millis now);  // timeout supervision

  Sensor* find(const std::string& id);
  const Sensor* find(const std::string& id) const;
  const std::vector<Sensor>& sensors() const { return sensors_; }
  std::vector<Sensor>& sensors() { return sensors_; }
  const std::vector<DiscoveredSensor>& discovered() const { return discovered_; }

  std::string upsert(const Sensor& cfg);
  bool remove(const std::string& id);
  std::string adopt(const std::string& id, const std::string& name, const std::string& room, SensorRole role);

  // Calibration: sensors placed side by side (or with a reference
  // thermometer) get offsets so that all show the same value.
  // reference: "mean" | "median" (of the raw values), "value" (refValue,
  // e.g. a calibrated thermometer), "reset" (all offsets 0) or the id of one
  // of the sensors (its corrected value is the target, its offset stays).
  // All or nothing: every sensor needs a current value and the resulting
  // offset must stay within +-10 K.
  struct CalibrationResult {
    std::string id;
    float raw = 0.f, oldOffset = 0.f, newOffset = 0.f;
  };
  std::string calibrate(const std::vector<std::string>& ids, const std::string& reference, float refValue,
                        std::vector<CalibrationResult>& out, float* target = nullptr);

  RoomTemperature roomTemperature(const std::string& room) const;
  bool firstOfRole(SensorRole role, float& value, std::string* id = nullptr) const;
  size_t countAvailable() const;

  bool autoAdoptOneWire = false;

  void toJson(JsonArray arr, bool runtime) const;
  std::string fromJson(JsonArrayConst arr);

  uint32_t revision() const { return revision_; }

 private:
  void setStatus(Sensor& s, SensorStatus st, Millis now);
  void noteDiscovered(const std::string& id, float v, bool hasHum, float hum, int8_t battery, int8_t batteryPct,
                      Millis now);
  EventBus& bus_;
  const IClock& clock_;
  std::vector<Sensor> sensors_;
  std::vector<DiscoveredSensor> discovered_;
  uint32_t revision_ = 0;
};

}  // namespace hc
