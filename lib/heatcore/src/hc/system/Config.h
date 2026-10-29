#pragma once
// System-, network- and MQTT configuration (plain data + JSON).
#include <ArduinoJson.h>

#include <cstdint>
#include <string>

namespace hc {

struct SystemConfig {
  std::string deviceName = "HeatCore";
  std::string timezone = "CET-1CEST,M3.5.0,M10.5.0/3";  // POSIX TZ (Germany)
  std::string ntpServer = "pool.ntp.org";
  uint16_t controlIntervalSec = 10;
  bool outdoorFromSensor = true;
  std::string webUser = "admin";
  std::string webPassword;  // empty = no authentication (local network only!)
};

struct NetworkConfig {
  std::string hostname = "heatcore";
  bool ethernet = true;  // W5500 on the KC868-A16 V3
  bool wifi = true;
  std::string wifiSsid;
  std::string wifiPassword;
  std::string apPassword = "heatcore123";  // fallback access point when no network
  bool staticIp = false;
  std::string ip, gateway, subnet, dns;
};

struct MqttConfig {
  bool enabled = false;
  std::string host;
  uint16_t port = 1883;
  std::string user;
  std::string password;
  std::string baseTopic;  // empty = "heatcore/<deviceId>"
  bool discovery = true;
  std::string discoveryPrefix = "homeassistant";
  uint16_t stateIntervalSec = 60;
};

void systemConfigToJson(const SystemConfig& c, JsonObject o, bool includeSecrets);
std::string systemConfigFromJson(SystemConfig& c, JsonObjectConst o);
void networkConfigToJson(const NetworkConfig& c, JsonObject o, bool includeSecrets);
std::string networkConfigFromJson(NetworkConfig& c, JsonObjectConst o);
void mqttConfigToJson(const MqttConfig& c, JsonObject o, bool includeSecrets);
std::string mqttConfigFromJson(MqttConfig& c, JsonObjectConst o);

// Secrets are never returned by the API; this marker means "unchanged".
constexpr const char* kSecretMask = "********";

}  // namespace hc
