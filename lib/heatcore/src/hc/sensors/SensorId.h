#pragma once
// Sensor identity. A 1-Wire temperature sensor is identified by its 64-bit
// ROM code alone ("ow:28FF0A1B2C3D4E5F"): the terminal (bus) it is connected
// to does not matter, so a sensor can be moved to another terminal and keeps
// its name, room and correction. Ids of earlier versions contained the bus
// ("ow1:", "ow2:", "ow3:") and are converted on the fly.
#include <string>

namespace hc {

inline std::string canonicalSensorId(const std::string& id) {
  if (id.size() != 20 || id[0] != 'o' || id[1] != 'w' || id[2] < '1' || id[2] > '9' || id[3] != ':') return id;
  std::string rom = id.substr(4);
  for (auto& c : rom) {
    if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
    bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
    if (!hex) return id;
  }
  return "ow:" + rom;
}

// Rule references: a sensor id, or "sensor:<id>" in conditions/triggers.
inline std::string canonicalSensorRef(const std::string& ref) {
  if (ref.rfind("sensor:", 0) == 0) return "sensor:" + canonicalSensorId(ref.substr(7));
  return canonicalSensorId(ref);
}

}  // namespace hc
