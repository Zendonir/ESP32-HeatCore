#include "hc/rf/RfSensors.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>

namespace hc {
namespace rf {

std::string SensorReading::key() const {
  return "rf:" + decoder + ":" + std::to_string(id) + ":" + std::to_string(channel);
}

// ------------------------------------------------------------- Nexus-TH
// Pulse position modulation: every bit is the gap between two ~500 us pulses
// (1000 us = 0, 2000 us = 1), so a 36 bit row is 37 pulses and 36 gaps. The
// gap after the last pulse is the row gap (~4000 us) = segment boundary.
// Bits (MSB first): id[8] battery_ok[1] unknown[1] channel-1[2]
//                   temperature[12, signed, 0.1 C] const[4]=1111 humidity[8]

bool NexusThDecoder::decodeRow(const Pulse* p, size_t n, SensorReading& out, uint64_t& rowBits, uint8_t& rowLen) {
  if (n != 73) return false;
  uint64_t v = 0;
  float errSum = 0.f;
  for (size_t i = 0; i < 36; ++i) {
    const Pulse& h = p[2 * i];
    const Pulse& l = p[2 * i + 1];
    if (h.level != 1 || h.us < 250 || h.us > 900) return false;
    if (l.level != 0) return false;
    int bit;
    if (l.us >= 650 && l.us <= 1450) {
      bit = 0;
      errSum += std::fabs(l.us - 1000.f) / 450.f;
    } else if (l.us >= 1550 && l.us <= 2700) {
      bit = 1;
      errSum += std::fabs(l.us - 2000.f) / 600.f;
    } else {
      return false;
    }
    v = (v << 1) | static_cast<uint64_t>(bit);
  }
  const Pulse& last = p[72];
  if (last.level != 1 || last.us < 250 || last.us > 900) return false;
  if (((v >> 8) & 0xF) != 0xF) return false;  // constant nibble
  uint8_t id = static_cast<uint8_t>((v >> 28) & 0xFF);
  uint16_t raw = static_cast<uint16_t>((v >> 12) & 0xFFF);
  int16_t t = static_cast<int16_t>(raw & 0x800 ? raw | 0xF000 : raw);
  uint8_t hum = static_cast<uint8_t>(v & 0xFF);
  if (id == 0 && t == 0) return false;
  float tempC = t * 0.1f;
  if (tempC < -40.f || tempC > 70.f) return false;
  out = SensorReading{};
  out.decoder = name();
  out.id = id;
  out.channel = static_cast<uint8_t>(((v >> 24) & 0x3) + 1);
  out.batteryOk = ((v >> 27) & 1) ? 1 : 0;
  out.hasTemperature = true;
  out.temperatureC = tempC;
  // Temperature-only variants send 0; values > 100 are not humidity.
  out.hasHumidity = hum > 0 && hum <= 100;
  out.humidity = hum;
  float q = 100.f * (1.f - errSum / 36.f);
  out.quality = static_cast<uint8_t>(q < 0 ? 0 : q);
  rowBits = v;
  rowLen = 36;
  return true;
}

std::vector<Pulse> NexusThDecoder::encodeRow(uint8_t id, bool batteryOk, uint8_t channel, float tempC,
                                             uint8_t humidity) {
  int16_t t = static_cast<int16_t>(std::lround(tempC * 10.f));
  uint64_t v = 0;
  v = (v << 8) | id;
  v = (v << 1) | (batteryOk ? 1u : 0u);
  v = (v << 1) | 0u;
  v = (v << 2) | static_cast<uint64_t>((channel - 1) & 0x3);
  v = (v << 12) | static_cast<uint64_t>(static_cast<uint16_t>(t) & 0xFFF);
  v = (v << 4) | 0xFu;
  v = (v << 8) | humidity;
  std::vector<Pulse> out;
  for (int i = 35; i >= 0; --i) {
    bool bit = (v >> i) & 1u;
    out.push_back({500, 1});
    out.push_back({static_cast<uint16_t>(bit ? 2000 : 1000), 0});
  }
  out.push_back({500, 1});
  out.push_back({4000, 0});  // row gap
  return out;
}

// --------------------------------------------------------------- hub

void RfSensorHub::addDecoder(IRfSensorDecoder* d, bool enabled) { slots_.push_back({d, enabled}); }

bool RfSensorHub::setEnabled(const std::string& name, bool enabled) {
  for (auto& s : slots_) {
    if (name == s.decoder->name()) {
      s.enabled = enabled;
      return true;
    }
  }
  return false;
}

bool RfSensorHub::isEnabled(const std::string& name) const {
  for (auto& s : slots_)
    if (name == s.decoder->name()) return s.enabled;
  return false;
}

bool RfSensorHub::onSegment(const Pulse* p, size_t n, Millis now) {
  bool consumed = false;
  for (auto& slot : slots_) {
    if (!slot.enabled) continue;
    SensorReading r;
    uint64_t bits = 0;
    uint8_t len = 0;
    if (!slot.decoder->decodeRow(p, n, r, bits, len)) continue;
    consumed = true;
    ++slot.rows;
    // expire old entries
    recent_.erase(std::remove_if(recent_.begin(), recent_.end(),
                                 [&](const Recent& e) {
                                   return elapsed(now, e.first) > confirmWindowMs &&
                                          (!e.delivered || elapsed(now, e.lastDelivered) > dedupeMs);
                                 }),
                  recent_.end());
    Recent* e = nullptr;
    for (auto& x : recent_)
      if (x.bits == bits && x.len == len) e = &x;
    if (!e) {
      if (recent_.size() >= 16) recent_.erase(recent_.begin());
      recent_.push_back({bits, len, 0, now, 0, false});
      e = &recent_.back();
    }
    if (e->delivered && elapsed(now, e->lastDelivered) <= dedupeMs) {
      e->lastDelivered = now;  // still the same burst
      continue;
    }
    if (elapsed(now, e->first) > confirmWindowMs) {
      e->first = now;
      e->count = 0;
    }
    if (++e->count >= minRows) {
      e->delivered = true;
      e->lastDelivered = now;
      e->count = 0;
      ++slot.accepted;
      if (listener_) listener_(ctx_, r);
    }
  }
  return consumed;
}

void RfSensorHub::injectExternal(const SensorReading& r, Millis now) {
  (void)now;
  if (listener_) listener_(ctx_, r);
}

// ------------------------------------------------------------ rtl_433

bool parseRtl433Json(const char* line, SensorReading& out, std::string& err) {
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, line);
  if (e) {
    err = std::string("JSON: ") + e.c_str();
    return false;
  }
  const char* model = doc["model"] | "";
  if (!*model || doc["id"].isNull()) {
    err = "model/id fehlt";
    return false;
  }
  out = SensorReading{};
  std::string m(model);
  for (auto& c : m)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) c = '_';
  out.decoder = "rtl433_" + m.substr(0, 20);
  out.id = doc["id"] | 0u;
  out.channel = doc["channel"] | 0;
  if (!doc["temperature_C"].isNull()) {
    out.hasTemperature = true;
    out.temperatureC = doc["temperature_C"] | 0.f;
  } else if (!doc["temperature_F"].isNull()) {
    out.hasTemperature = true;
    out.temperatureC = ((doc["temperature_F"] | 32.f) - 32.f) * 5.f / 9.f;
  }
  if (!doc["humidity"].isNull()) {
    out.hasHumidity = true;
    out.humidity = doc["humidity"] | 0.f;
  }
  if (!doc["battery_ok"].isNull()) out.batteryOk = (doc["battery_ok"] | 0) ? 1 : 0;
  out.quality = 100;
  if (!out.hasTemperature) {
    err = "keine Temperatur im Telegramm";
    return false;
  }
  return true;
}

}  // namespace rf
}  // namespace hc
