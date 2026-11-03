#pragma once
// External data sources: values from other devices via MQTT subscriptions
// (e.g. a Shelly that receives Shelly BLU sensors over Bluetooth, a Button+
// with its temperature sensor and buttons, Zigbee2MQTT, Tasmota) or via HTTP
// polling of a device's local API (e.g. a Shelly without any MQTT broker).
//
// Every source has a short name. Depending on its kind it
//   * delivers a temperature to the sensor "ext:<name>" (appears under
//     "newly found" sensors and is assigned to a room like any sensor),
//   * adds humidity or battery state to such a temperature sensor, or
//   * raises the automation trigger "MQTT" with ref <name> (buttons).
// The payload is either a plain number ("21.3") or JSON; `path` selects a
// field with dots, e.g. "value" or "params.bthomesensor:200.value".
// Missing sources only make the sensor stale; control falls back as usual.
#include <ArduinoJson.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "hc/core/Clock.h"

namespace hc {

class HeatCoreSystem;

enum class ExtKind : uint8_t { Temperature = 0, Humidity, Battery, Trigger };
const char* extKindName(ExtKind k);

struct ExtSource {
  std::string name;        // [a-z0-9_-], 1..20
  bool http = false;       // false: MQTT topic, true: HTTP URL polled
  std::string topic;       // MQTT topic (no wildcards) or http:// URL
  ExtKind kind = ExtKind::Temperature;
  std::string path;        // JSON field path, empty = plain payload
  std::string sensor;      // humidity/battery: name of the temperature source
  uint16_t intervalSec = 60;  // HTTP polling interval
};

struct ExtStatus {
  std::string last;  // last extracted value (text)
  uint32_t count = 0;
  Millis lastMs = 0;
  bool seen = false;
  std::string error;
  Millis lastPollMs = 0;
  bool polled = false;
  // temperature sources: latest humidity / battery from their companions
  bool hasHum = false;
  float hum = 0.f;
  int8_t battery = -1;
};

class ExternalSources {
 public:
  static constexpr size_t kMax = 32;
  std::vector<ExtSource> list;

  std::string fromJson(JsonArrayConst a);
  void toJson(JsonArray a) const;
  void statusJson(JsonArray a, Millis now) const;

  std::vector<std::string> mqttTopics() const;
  // MQTT message: handles every source with this topic; true if any matched.
  bool ingestMqtt(HeatCoreSystem& sys, const std::string& topic, const std::string& payload, Millis now);
  // HTTP sources due for polling (indices); marks them as polled.
  std::vector<size_t> dueHttp(Millis now);
  void ingestHttp(HeatCoreSystem& sys, size_t idx, int httpStatus, const std::string& body, Millis now);

  // Value extraction (exposed for tests). Returns false if the path is
  // missing. `num` is set when the value is numeric.
  static bool extract(const std::string& payload, const std::string& path, std::string& text, bool& isNum,
                      float& num);
  static std::string sensorId(const std::string& name) { return "ext:" + name; }

 private:
  void apply(HeatCoreSystem& sys, size_t idx, const std::string& payload, Millis now);
  std::vector<ExtStatus> status_;
};

}  // namespace hc
