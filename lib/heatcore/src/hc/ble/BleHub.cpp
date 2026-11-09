#include "hc/ble/BleHub.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "hc/sensors/SensorManager.h"
#include "hc/system/Config.h"

namespace hc {
namespace ble {

namespace {
constexpr const char* kMask = kSecretMask;

uint8_t qualityFromRssi(int rssi) {
  // -100 dBm (barely received) .. -40 dBm (next to the receiver)
  int q = (rssi + 100) * 100 / 60;
  return static_cast<uint8_t>(std::min(100, std::max(0, q)));
}
}  // namespace

const Key* BleHub::keyFor(const Mac& m) const {
  for (auto& k : cfg.keys)
    if (k.first == m) return &k.second;
  return nullptr;
}

BleDevice& BleHub::device(const Mac& m, Millis now) {
  for (auto& d : devices_)
    if (d.mac == m) return d;
  if (devices_.size() >= kMaxDevices) {
    // forget the device not heard of for the longest time
    auto oldest = std::min_element(devices_.begin(), devices_.end(), [&](const BleDevice& a, const BleDevice& b) {
      return elapsed(now, a.lastMs) > elapsed(now, b.lastMs);
    });
    devices_.erase(oldest);
  }
  devices_.emplace_back();
  devices_.back().mac = m;
  return devices_.back();
}

void BleHub::ingest(const Mac& advMac, int rssi, const uint8_t* adv, size_t len, Millis now) {
  if (!cfg.enabled) return;
  ++stats.adverts;
  Reading r;
  KeyLookup keys = [this](const Mac& m) { return keyFor(m); };
  if (!decode(advMac, adv, len, keys, r)) return;
  const Mac mac = r.hasDeviceMac ? r.deviceMac : advMac;  // Xiaomi names itself in the frame
  BleDevice& d = device(mac, now);
  ++d.adverts;
  d.lastMs = now;
  d.rssi = rssi;
  d.format = r.format;
  if (!r.name.empty()) d.name = r.name;
  d.encrypted = r.encrypted;
  d.needsKey = r.needsKey;
  d.keyError = r.keyError;
  d.unsupported = r.unsupported;
  if (r.keyError) ++stats.keyErrors;
  // Xiaomi sends temperature, humidity and battery in separate messages
  if (r.hasTemp) d.temp = r.temp, d.hasTemp = true;
  if (r.hasHum) d.hum = r.hum, d.hasHum = true;
  if (r.batteryPct >= 0) d.batteryPct = r.batteryPct;
  bool repeat = r.counter >= 0 && r.counter == d.lastCounter;  // same message sent several times
  if (r.counter >= 0) d.lastCounter = r.counter;
  if (!d.hasTemp || !(r.hasTemp || r.hasHum || r.batteryPct >= 0) || repeat) return;
  if (d.forwarded && elapsed(now, d.lastForwardMs) < kForwardIntervalMs) return;
  d.forwarded = true;
  d.lastForwardMs = now;
  ++stats.readings;
  int8_t batOk = d.batteryPct < 0 ? -1 : (d.batteryPct >= 15 ? 1 : 0);
  sensors_.updateExtended(sensorId(mac), d.temp, d.hasHum, d.hum, batOk, qualityFromRssi(rssi), now,
                          static_cast<int8_t>(d.batteryPct));
}

void BleHub::tick(Millis now) {
  devices_.erase(std::remove_if(devices_.begin(), devices_.end(),
                                [&](const BleDevice& d) { return elapsed(now, d.lastMs) > kForgetMs; }),
                 devices_.end());
}

std::string BleHub::setKey(const std::string& macStr, const std::string& keyStr) {
  Mac m;
  if (!parseMac(macStr, m)) return "MAC-Adresse ungueltig";
  auto it = std::find_if(cfg.keys.begin(), cfg.keys.end(), [&](const std::pair<Mac, Key>& k) { return k.first == m; });
  if (keyStr.empty()) {
    if (it != cfg.keys.end()) cfg.keys.erase(it);
  } else {
    Key k;
    if (!parseKey(keyStr, k)) return "Schluessel muss aus 32 Hex-Zeichen bestehen";
    if (it != cfg.keys.end()) it->second = k;
    else if (cfg.keys.size() >= kMaxDevices) return "Zu viele Schluessel";
    else cfg.keys.emplace_back(m, k);
  }
  for (auto& d : devices_)
    if (d.mac == m) d.needsKey = d.keyError = false, d.lastCounter = -1;
  ++revision_;
  return std::string();
}

void BleHub::toJson(JsonObject o, bool includeSecrets) const {
  o["enabled"] = cfg.enabled;
  JsonArray a = o["keys"].to<JsonArray>();
  for (auto& k : cfg.keys) {
    JsonObject x = a.add<JsonObject>();
    x["mac"] = macHex(k.first);
    if (includeSecrets) {
      char hex[33];
      for (int i = 0; i < 16; ++i) std::snprintf(hex + 2 * i, 3, "%02x", k.second[static_cast<size_t>(i)]);
      x["key"] = hex;
    } else {
      x["key"] = kMask;
    }
  }
}

std::string BleHub::fromJson(JsonObjectConst o) {
  BleConfig n;
  n.enabled = o["enabled"] | false;
  for (JsonObjectConst x : o["keys"].as<JsonArrayConst>()) {
    Mac m;
    if (!parseMac(x["mac"] | "", m)) return "Bluetooth: MAC-Adresse ungueltig";
    const char* ks = x["key"] | "";
    Key k;
    if (!std::strcmp(ks, kMask)) {  // unchanged
      const Key* old = keyFor(m);
      if (!old) continue;
      k = *old;
    } else if (!parseKey(ks, k)) {
      return "Bluetooth: Schluessel fuer " + macHex(m) + " muss aus 32 Hex-Zeichen bestehen";
    }
    if (n.keys.size() < kMaxDevices) n.keys.emplace_back(m, k);
  }
  cfg = std::move(n);
  ++revision_;
  return std::string();
}

void BleHub::statusJson(JsonObject o, Millis now) const {
  o["enabled"] = cfg.enabled;
  o["available"] = radioAvailable;
  o["running"] = running;
  if (!radioError.empty()) o["error"] = radioError;
  JsonObject st = o["stats"].to<JsonObject>();
  st["adverts"] = stats.adverts;
  st["readings"] = stats.readings;
  st["keyErrors"] = stats.keyErrors;
  st["dropped"] = stats.dropped;
  JsonArray a = o["devices"].to<JsonArray>();
  for (auto& d : devices_) {
    JsonObject x = a.add<JsonObject>();
    std::string id = sensorId(d.mac);
    x["mac"] = macHex(d.mac);
    x["id"] = id;
    x["format"] = formatName(d.format);
    if (!d.name.empty()) x["name"] = d.name;
    x["rssi"] = d.rssi;
    x["ageSec"] = elapsed(now, d.lastMs) / 1000u;
    if (d.hasTemp) x["temp"] = std::round(d.temp * 100.f) / 100.f;
    if (d.hasHum) x["hum"] = std::round(d.hum * 10.f) / 10.f;
    if (d.batteryPct >= 0) x["battery"] = d.batteryPct;
    x["encrypted"] = d.encrypted;
    x["hasKey"] = keyFor(d.mac) != nullptr;
    if (d.needsKey) x["needsKey"] = true;
    if (d.keyError) x["keyError"] = true;
    if (d.unsupported) x["unsupported"] = true;
    x["adopted"] = sensors_.find(id) != nullptr;
  }
}

}  // namespace ble
}  // namespace hc
