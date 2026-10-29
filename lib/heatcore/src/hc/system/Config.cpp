#include "hc/system/Config.h"

#include <cstring>

namespace hc {

namespace {
void secret(JsonObject o, const char* key, const std::string& v, bool include) {
  if (include) o[key] = v;
  else o[key] = v.empty() ? "" : kSecretMask;
}
void readSecret(JsonObjectConst o, const char* key, std::string& v) {
  if (o[key].isNull()) return;
  const char* s = o[key] | "";
  if (std::strcmp(s, kSecretMask) == 0) return;  // unchanged
  v = s;
}
bool validHost(const std::string& h) {
  if (h.empty() || h.size() > 31) return false;
  for (char c : h)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  return true;
}
}  // namespace

void systemConfigToJson(const SystemConfig& c, JsonObject o, bool includeSecrets) {
  o["deviceName"] = c.deviceName;
  o["timezone"] = c.timezone;
  o["ntpServer"] = c.ntpServer;
  o["controlIntervalSec"] = c.controlIntervalSec;
  o["outdoorFromSensor"] = c.outdoorFromSensor;
  o["webUser"] = c.webUser;
  secret(o, "webPassword", c.webPassword, includeSecrets);
  o["updateUrl"] = c.updateUrl;
}

std::string systemConfigFromJson(SystemConfig& c, JsonObjectConst o) {
  SystemConfig n = c;
  n.deviceName = o["deviceName"] | n.deviceName.c_str();
  n.timezone = o["timezone"] | n.timezone.c_str();
  n.ntpServer = o["ntpServer"] | n.ntpServer.c_str();
  n.controlIntervalSec = o["controlIntervalSec"] | n.controlIntervalSec;
  n.outdoorFromSensor = o["outdoorFromSensor"] | n.outdoorFromSensor;
  n.webUser = o["webUser"] | n.webUser.c_str();
  readSecret(o, "webPassword", n.webPassword);
  n.updateUrl = o["updateUrl"] | n.updateUrl.c_str();
  if (!n.updateUrl.empty() && n.updateUrl.back() != '/') n.updateUrl += '/';
  if (!n.updateUrl.empty() && (n.updateUrl.rfind("https://", 0) != 0 || n.updateUrl.size() > 200))
    return "Update-Quelle: https://-Adresse (max. 200 Zeichen) oder leer";
  if (n.deviceName.empty() || n.deviceName.size() > 31) return "Geraetename 1..31 Zeichen";
  if (n.controlIntervalSec < 2 || n.controlIntervalSec > 60) return "Regelintervall 2..60 s";
  if (n.timezone.size() > 63) return "Zeitzone zu lang";
  c = n;
  return std::string();
}

void networkConfigToJson(const NetworkConfig& c, JsonObject o, bool includeSecrets) {
  o["hostname"] = c.hostname;
  o["ethernet"] = c.ethernet;
  o["wifi"] = c.wifi;
  o["wifiSsid"] = c.wifiSsid;
  secret(o, "wifiPassword", c.wifiPassword, includeSecrets);
  secret(o, "apPassword", c.apPassword, includeSecrets);
  o["staticIp"] = c.staticIp;
  o["ip"] = c.ip;
  o["gateway"] = c.gateway;
  o["subnet"] = c.subnet;
  o["dns"] = c.dns;
}

std::string networkConfigFromJson(NetworkConfig& c, JsonObjectConst o) {
  NetworkConfig n = c;
  n.hostname = o["hostname"] | n.hostname.c_str();
  n.ethernet = o["ethernet"] | n.ethernet;
  n.wifi = o["wifi"] | n.wifi;
  n.wifiSsid = o["wifiSsid"] | n.wifiSsid.c_str();
  readSecret(o, "wifiPassword", n.wifiPassword);
  readSecret(o, "apPassword", n.apPassword);
  n.staticIp = o["staticIp"] | n.staticIp;
  n.ip = o["ip"] | n.ip.c_str();
  n.gateway = o["gateway"] | n.gateway.c_str();
  n.subnet = o["subnet"] | n.subnet.c_str();
  n.dns = o["dns"] | n.dns.c_str();
  if (!validHost(n.hostname)) return "Hostname: 1..31 Zeichen a-z, 0-9, -";
  if (n.apPassword.size() < 8 || n.apPassword.size() > 63) return "AP-Passwort 8..63 Zeichen";
  if (n.staticIp && (n.ip.empty() || n.gateway.empty() || n.subnet.empty())) return "Statische IP unvollstaendig";
  c = n;
  return std::string();
}

void mqttConfigToJson(const MqttConfig& c, JsonObject o, bool includeSecrets) {
  o["enabled"] = c.enabled;
  o["host"] = c.host;
  o["port"] = c.port;
  o["user"] = c.user;
  secret(o, "password", c.password, includeSecrets);
  o["baseTopic"] = c.baseTopic;
  o["discovery"] = c.discovery;
  o["discoveryPrefix"] = c.discoveryPrefix;
  o["stateIntervalSec"] = c.stateIntervalSec;
}

std::string mqttConfigFromJson(MqttConfig& c, JsonObjectConst o) {
  MqttConfig n = c;
  n.enabled = o["enabled"] | n.enabled;
  n.host = o["host"] | n.host.c_str();
  n.port = o["port"] | n.port;
  n.user = o["user"] | n.user.c_str();
  readSecret(o, "password", n.password);
  n.baseTopic = o["baseTopic"] | n.baseTopic.c_str();
  n.discovery = o["discovery"] | n.discovery;
  n.discoveryPrefix = o["discoveryPrefix"] | n.discoveryPrefix.c_str();
  n.stateIntervalSec = o["stateIntervalSec"] | n.stateIntervalSec;
  if (n.enabled && n.host.empty()) return "MQTT-Broker fehlt";
  if (n.port == 0) return "MQTT-Port ungueltig";
  if (n.baseTopic.find_first_of("#+") != std::string::npos) return "Basisthema ohne Platzhalter";
  if (n.stateIntervalSec < 10 || n.stateIntervalSec > 3600) return "Statusintervall 10..3600 s";
  c = n;
  return std::string();
}

}  // namespace hc
