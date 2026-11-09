#pragma once
// Bluetooth LE thermometers: receives advertisements from the platform
// scanner, decodes them (BleDecoders), keeps a list of the thermometers in
// range and forwards their readings to the sensor manager as sensors
// "ble:<MAC>". Adopted ("paired") sensors are then handled like every other
// sensor: rooms, calibration, MQTT / Home Assistant.
#include <ArduinoJson.h>

#include <string>
#include <utility>
#include <vector>

#include "hc/ble/BleDecoders.h"
#include "hc/core/Clock.h"

namespace hc {

class SensorManager;

namespace ble {

struct BleConfig {
  bool enabled = false;
  std::vector<std::pair<Mac, Key>> keys;  // bind keys of encrypted devices
};

struct BleDevice {
  Mac mac{};
  Format format = Format::None;
  std::string name;
  int rssi = 0;
  bool encrypted = false, needsKey = false, keyError = false, unsupported = false;
  bool hasTemp = false, hasHum = false;
  float temp = 0.f, hum = 0.f;
  int batteryPct = -1;
  int64_t lastCounter = -1;
  uint32_t adverts = 0;
  Millis lastMs = 0;
  Millis tempMs = 0;         // reception of the last new temperature
  Millis lastForwardMs = 0;
  bool forwarded = false;
  bool newTemp = false;      // a temperature not yet forwarded (waits for the interval)
};

struct BleStats {
  uint32_t adverts = 0;   // candidate advertisements received
  uint32_t readings = 0;  // values forwarded to the sensor manager
  uint32_t keyErrors = 0;
  uint32_t dropped = 0;   // set by the platform (queue full)
};

class BleHub {
 public:
  static constexpr size_t kMaxDevices = 32;
  static constexpr uint32_t kForwardIntervalMs = 30000;  // per device
  static constexpr uint32_t kForgetMs = 3600u * 1000u;

  explicit BleHub(SensorManager& sensors) : sensors_(sensors) {}

  BleConfig cfg;
  BleStats stats;
  // reported by the platform layer
  bool radioAvailable = false;  // firmware built with Bluetooth support
  bool running = false;         // scanner active
  std::string radioError;

  // One advertisement (AD structures) from the scanner.
  void ingest(const Mac& mac, int rssi, const uint8_t* adv, size_t len, Millis now);
  void tick(Millis now);

  const std::vector<BleDevice>& devices() const { return devices_; }
  static std::string sensorId(const Mac& m) { return "ble:" + macHex(m); }
  const Key* keyFor(const Mac& m) const;
  // empty key removes the entry
  std::string setKey(const std::string& mac, const std::string& key);

  void toJson(JsonObject o, bool includeSecrets) const;  // configuration
  std::string fromJson(JsonObjectConst o);
  void statusJson(JsonObject o, Millis now) const;       // runtime for the web UI

  uint32_t revision() const { return revision_; }

 private:
  BleDevice& device(const Mac& m, Millis now);
  void flush(BleDevice& d, Millis now);
  SensorManager& sensors_;
  std::vector<BleDevice> devices_;
  uint32_t revision_ = 0;
};

}  // namespace ble
}  // namespace hc
