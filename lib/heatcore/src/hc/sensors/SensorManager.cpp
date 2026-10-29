#include "hc/sensors/SensorManager.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "hc/core/Log.h"

namespace hc {

namespace {
constexpr const char* TAG = "sensors";
}

const char* sensorKindName(SensorKind k) {
  switch (k) {
    case SensorKind::OneWire: return "onewire";
    case SensorKind::Rf: return "rf";
    case SensorKind::External: return "external";
    case SensorKind::Analog: return "analog";
    case SensorKind::Virtual: return "virtual";
  }
  return "?";
}

SensorKind sensorKindFromKey(const std::string& id) {
  if (id.rfind("ow", 0) == 0) return SensorKind::OneWire;
  if (id.rfind("rf:rtl433", 0) == 0) return SensorKind::External;
  if (id.rfind("rf:", 0) == 0) return SensorKind::Rf;
  if (id.rfind("ai:", 0) == 0) return SensorKind::Analog;
  if (id.rfind("ext:", 0) == 0) return SensorKind::External;
  return SensorKind::Virtual;
}

const char* sensorRoleName(SensorRole r) {
  switch (r) {
    case SensorRole::Room: return "room";
    case SensorRole::Flow: return "flow";
    case SensorRole::Return: return "return";
    case SensorRole::Outdoor: return "outdoor";
    case SensorRole::Other: return "other";
  }
  return "?";
}

SensorRole sensorRoleFromName(const char* s) {
  if (!s) return SensorRole::Room;
  if (!std::strcmp(s, "flow")) return SensorRole::Flow;
  if (!std::strcmp(s, "return")) return SensorRole::Return;
  if (!std::strcmp(s, "outdoor")) return SensorRole::Outdoor;
  if (!std::strcmp(s, "other")) return SensorRole::Other;
  return SensorRole::Room;
}

const char* sensorStatusName(SensorStatus s) {
  switch (s) {
    case SensorStatus::Never: return "never";
    case SensorStatus::Ok: return "ok";
    case SensorStatus::Stale: return "stale";
    case SensorStatus::Fault: return "fault";
    case SensorStatus::Disabled: return "disabled";
  }
  return "?";
}

Sensor* SensorManager::find(const std::string& id) {
  for (auto& s : sensors_)
    if (s.id == id) return &s;
  return nullptr;
}
const Sensor* SensorManager::find(const std::string& id) const {
  for (auto& s : sensors_)
    if (s.id == id) return &s;
  return nullptr;
}

void SensorManager::setStatus(Sensor& s, SensorStatus st, Millis now) {
  if (s.status == st) return;
  SensorStatus old = s.status;
  s.status = st;
  if (st == SensorStatus::Stale || st == SensorStatus::Fault) {
    Event e(EventType::SensorFault, EventSource::Sensor);
    e.ts = now;
    e.setRef(s.id.c_str());
    e.setText(st == SensorStatus::Stale ? "timeout" : s.lastError.c_str());
    bus_.post(e);
    HC_LOGW(TAG, "%s -> %s", s.id.c_str(), sensorStatusName(st));
  } else if (st == SensorStatus::Ok && (old == SensorStatus::Stale || old == SensorStatus::Fault)) {
    Event e(EventType::SensorRecovered, EventSource::Sensor);
    e.ts = now;
    e.setRef(s.id.c_str());
    e.value = s.value;
    bus_.post(e);
  }
}

void SensorManager::update(const std::string& id, float raw, Millis now) {
  updateExtended(id, raw, false, 0.f, -1, 100, now);
}

void SensorManager::updateExtended(const std::string& id, float raw, bool hasHum, float hum, int8_t battery,
                                   uint8_t quality, Millis now) {
  Sensor* s = find(id);
  if (!s) {
    SensorKind k = sensorKindFromKey(id);
    if (k == SensorKind::OneWire && autoAdoptOneWire && sensors_.size() < kMaxSensors) {
      Sensor n;
      n.id = id;
      n.name = id;
      n.kind = k;
      n.configuredMs = now;
      sensors_.push_back(n);
      ++revision_;
      s = &sensors_.back();
    } else {
      noteDiscovered(id, raw, hasHum, hum, battery, now);
      return;
    }
  }
  if (!s->enabled) {
    s->status = SensorStatus::Disabled;
    return;
  }
  // DS18B20 delivers 85.0 after a power glitch and -127 when disconnected.
  bool implausible = !std::isfinite(raw) || raw < s->minValid || raw > s->maxValid ||
                     (s->kind == SensorKind::OneWire && (raw == 85.0f || raw <= -127.f));
  if (implausible) {
    ++s->errors;
    s->lastError = "unplausibler Messwert";
    // keep the last valid value; status changes only via timeout unless the
    // sensor never delivered a valid value
    if (s->status == SensorStatus::Never) setStatus(*s, SensorStatus::Fault, now);
    return;
  }
  s->raw = raw;
  s->value = raw + s->offset;
  s->hasHumidity = hasHum;
  s->humidity = hum;
  if (battery >= 0) s->batteryOk = battery;
  s->quality = quality;
  s->lastUpdateMs = now;
  s->lastEpoch = clock_.epoch();
  ++s->updates;
  setStatus(*s, SensorStatus::Ok, now);
  Event e(EventType::SensorValue, EventSource::Sensor);
  e.ts = now;
  e.setRef(s->id.c_str());
  e.value = s->value;
  bus_.post(e);
}

void SensorManager::reportError(const std::string& id, const char* err, Millis now) {
  Sensor* s = find(id);
  if (!s || !s->enabled) return;
  ++s->errors;
  s->lastError = err ? err : "Fehler";
  if (s->status == SensorStatus::Never) setStatus(*s, SensorStatus::Fault, now);
}

void SensorManager::tick(Millis now) {
  for (auto& s : sensors_) {
    if (!s.enabled) {
      s.status = SensorStatus::Disabled;
      continue;
    }
    if (s.status == SensorStatus::Disabled) s.status = SensorStatus::Never;
    if (s.status == SensorStatus::Ok && s.timeoutSec && elapsed(now, s.lastUpdateMs) > s.timeoutSec * 1000u) {
      ++s.timeouts;
      s.lastError = "Zeitueberschreitung";
      setStatus(s, SensorStatus::Stale, now);
    }
    // a configured sensor that never reported within its timeout is a fault
    if (s.status == SensorStatus::Never && s.timeoutSec && elapsed(now, s.configuredMs) > s.timeoutSec * 1000u) {
      s.lastError = "keine Daten seit Start";
      setStatus(s, SensorStatus::Fault, now);
    }
  }
  discovered_.erase(std::remove_if(discovered_.begin(), discovered_.end(),
                                   [&](const DiscoveredSensor& d) { return elapsed(now, d.lastMs) > 3600u * 1000u; }),
                    discovered_.end());
}

void SensorManager::noteDiscovered(const std::string& id, float v, bool hasHum, float hum, int8_t battery,
                                   Millis now) {
  for (auto& d : discovered_) {
    if (d.id == id) {
      d.lastValue = v;
      d.hasHumidity = hasHum;
      d.humidity = hum;
      d.batteryOk = battery;
      ++d.seen;
      d.lastMs = now;
      return;
    }
  }
  if (discovered_.size() >= kMaxDiscovered) discovered_.erase(discovered_.begin());
  discovered_.push_back({id, sensorKindFromKey(id), v, hasHum, hum, battery, 1, now});
}

std::string SensorManager::upsert(const Sensor& cfg) {
  if (cfg.id.empty() || cfg.id.size() > 40) return "Sensor-ID ungueltig";
  if (cfg.name.size() > 31) return "Name zu lang";
  if (std::fabs(cfg.offset) > 10.f) return "Korrektur max. +-10 K";
  if (cfg.timeoutSec != 0 && (cfg.timeoutSec < 30 || cfg.timeoutSec > 86400)) return "Timeout 0 oder 30..86400 s";
  if (cfg.fallbackId == cfg.id) return "Sensor kann nicht sein eigener Ersatz sein";
  if (!cfg.fallbackId.empty()) {
    // reject fallback cycles
    std::string cur = cfg.fallbackId;
    for (int depth = 0; depth < 8 && !cur.empty(); ++depth) {
      if (cur == cfg.id) return "Ersatzsensor-Kette bildet eine Schleife";
      const Sensor* f = find(cur);
      if (!f) break;
      cur = f->fallbackId;
    }
  }
  Sensor* s = find(cfg.id);
  if (!s) {
    if (sensors_.size() >= kMaxSensors) return "Maximale Sensoranzahl erreicht";
    sensors_.push_back(cfg);
    s = &sensors_.back();
    s->kind = sensorKindFromKey(cfg.id);
    s->status = SensorStatus::Never;
    s->configuredMs = clock_.nowMs();
    discovered_.erase(std::remove_if(discovered_.begin(), discovered_.end(),
                                     [&](const DiscoveredSensor& d) { return d.id == cfg.id; }),
                      discovered_.end());
  } else {
    float oldRaw = s->raw;
    bool wasValid = s->status == SensorStatus::Ok;
    s->name = cfg.name;
    s->role = cfg.role;
    s->room = cfg.room;
    s->offset = cfg.offset;
    s->timeoutSec = cfg.timeoutSec;
    s->priority = cfg.priority;
    s->fallbackId = cfg.fallbackId;
    s->enabled = cfg.enabled;
    s->minValid = cfg.minValid;
    s->maxValid = cfg.maxValid;
    if (wasValid) s->value = oldRaw + s->offset;
  }
  ++revision_;
  return std::string();
}

std::string SensorManager::adopt(const std::string& id, const std::string& name, const std::string& room,
                                 SensorRole role) {
  Sensor s;
  s.id = id;
  s.name = name.empty() ? id : name;
  s.room = room;
  s.role = role;
  s.kind = sensorKindFromKey(id);
  if (s.kind == SensorKind::Rf || s.kind == SensorKind::External) s.timeoutSec = 900;  // RF sensors send rarely
  // keep the last discovered reading: upsert() removes the discovery entry
  bool haveLast = false;
  DiscoveredSensor last;
  for (auto& d : discovered_)
    if (d.id == id) {
      last = d;
      haveLast = true;
    }
  std::string err = upsert(s);
  if (!err.empty()) return err;
  if (haveLast) updateExtended(id, last.lastValue, last.hasHumidity, last.humidity, last.batteryOk, 100, last.lastMs);
  return std::string();
}

bool SensorManager::remove(const std::string& id) {
  auto it = std::remove_if(sensors_.begin(), sensors_.end(), [&](const Sensor& s) { return s.id == id; });
  if (it == sensors_.end()) return false;
  sensors_.erase(it, sensors_.end());
  for (auto& s : sensors_)
    if (s.fallbackId == id) s.fallbackId.clear();
  ++revision_;
  return true;
}

RoomTemperature SensorManager::roomTemperature(const std::string& room) const {
  RoomTemperature rt;
  const Sensor* best = nullptr;
  const Sensor* primary = nullptr;  // best configured sensor regardless of status
  for (auto& s : sensors_) {
    if (!s.enabled || s.room != room || s.role != SensorRole::Room) continue;
    if (!primary || s.priority > primary->priority) primary = &s;
    if (s.available() && (!best || s.priority > best->priority)) best = &s;
  }
  if (best) {
    rt.valid = true;
    rt.value = best->value;
    rt.sensorId = best->id;
    rt.fallback = best != primary;
    return rt;
  }
  // follow the fallback chain of the primary sensor (may point to another room)
  if (primary) {
    std::string cur = primary->fallbackId;
    for (int depth = 0; depth < 8 && !cur.empty(); ++depth) {
      const Sensor* f = find(cur);
      if (!f) break;
      if (f->enabled && f->available()) {
        rt.valid = true;
        rt.value = f->value;
        rt.sensorId = f->id;
        rt.fallback = true;
        return rt;
      }
      cur = f->fallbackId;
    }
  }
  return rt;
}

bool SensorManager::firstOfRole(SensorRole role, float& value, std::string* id) const {
  const Sensor* best = nullptr;
  for (auto& s : sensors_)
    if (s.enabled && s.role == role && s.available() && (!best || s.priority > best->priority)) best = &s;
  if (!best) return false;
  value = best->value;
  if (id) *id = best->id;
  return true;
}

size_t SensorManager::countAvailable() const {
  size_t n = 0;
  for (auto& s : sensors_)
    if (s.available()) ++n;
  return n;
}

void SensorManager::toJson(JsonArray arr, bool runtime) const {
  for (auto& s : sensors_) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = s.id;
    o["name"] = s.name;
    o["kind"] = sensorKindName(s.kind);
    o["role"] = sensorRoleName(s.role);
    o["room"] = s.room;
    o["offset"] = s.offset;
    o["timeoutSec"] = s.timeoutSec;
    o["priority"] = s.priority;
    o["fallbackId"] = s.fallbackId;
    o["enabled"] = s.enabled;
    o["minValid"] = s.minValid;
    o["maxValid"] = s.maxValid;
    if (runtime) {
      o["status"] = sensorStatusName(s.status);
      if (s.status != SensorStatus::Never) {
        o["value"] = s.value;
        o["raw"] = s.raw;
      }
      if (s.hasHumidity) o["humidity"] = s.humidity;
      if (s.batteryOk >= 0) o["batteryOk"] = s.batteryOk == 1;
      o["quality"] = s.quality;
      o["lastEpoch"] = s.lastEpoch;
      o["ageSec"] = s.updates ? elapsed(clock_.nowMs(), s.lastUpdateMs) / 1000u : 0u;
      o["updates"] = s.updates;
      o["errors"] = s.errors;
      o["timeouts"] = s.timeouts;
      o["lastError"] = s.lastError;
    }
  }
}

std::string SensorManager::fromJson(JsonArrayConst arr) {
  std::vector<Sensor> list;
  for (JsonObjectConst o : arr) {
    Sensor s;
    s.id = o["id"] | "";
    if (s.id.empty()) continue;
    s.name = o["name"] | s.id.c_str();
    s.kind = sensorKindFromKey(s.id);
    s.role = sensorRoleFromName(o["role"] | "room");
    s.room = o["room"] | "";
    s.offset = o["offset"] | 0.f;
    s.timeoutSec = o["timeoutSec"] | 300u;
    s.priority = o["priority"] | 50;
    s.fallbackId = o["fallbackId"] | "";
    s.enabled = o["enabled"] | true;
    s.minValid = o["minValid"] | -40.f;
    s.maxValid = o["maxValid"] | 100.f;
    s.configuredMs = clock_.nowMs();
    list.push_back(s);
  }
  sensors_ = std::move(list);
  ++revision_;
  return std::string();
}

}  // namespace hc
