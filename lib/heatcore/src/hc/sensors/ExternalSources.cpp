#include "hc/sensors/ExternalSources.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hc/system/HeatCoreSystem.h"

namespace hc {

namespace {

const char* const kKindNames[] = {"temperature", "humidity", "battery", "trigger"};

bool validName(const std::string& n) {
  if (n.empty() || n.size() > 20) return false;
  for (char c : n)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  return true;
}

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Plain number, also with a decimal comma and a unit suffix ("21,3 °C").
bool parseNumber(const std::string& in, float& out) {
  std::string s = trim(in);
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  std::replace(s.begin(), s.end(), ',', '.');
  if (s.empty()) return false;
  char* end = nullptr;
  float v = std::strtof(s.c_str(), &end);
  if (end == s.c_str() || !std::isfinite(v)) return false;
  while (*end == ' ') ++end;
  if (*end && !(static_cast<unsigned char>(*end) >= 0x80 || std::strchr("%CcFfKV", *end))) return false;
  out = v;
  return true;
}

uint32_t elapsedMs(Millis now, Millis since) { return elapsed(now, since); }

}  // namespace

const char* extKindName(ExtKind k) { return kKindNames[static_cast<int>(k)]; }

// ------------------------------------------------------------ config

std::string ExternalSources::fromJson(JsonArrayConst a) {
  std::vector<ExtSource> n;
  for (JsonObjectConst x : a) {
    ExtSource s;
    s.name = x["name"] | "";
    std::string pre = "Externe Quelle '" + s.name + "': ";
    if (!validName(s.name)) return "Externe Quelle: Name 1..20 Zeichen a-z, 0-9, _ -";
    for (auto& o : n)
      if (o.name == s.name) return pre + "Name doppelt";
    s.http = std::string(x["source"] | "mqtt") == "http";
    s.topic = x["topic"] | "";
    std::string kind = x["kind"] | "temperature";
    bool kindOk = false;
    for (int k = 0; k < 4; ++k)
      if (kind == kKindNames[k]) {
        s.kind = static_cast<ExtKind>(k);
        kindOk = true;
      }
    if (!kindOk) return pre + "Art temperature|humidity|battery|trigger";
    s.path = x["path"] | "";
    s.sensor = x["sensor"] | "";
    s.intervalSec = x["intervalSec"] | 60;
    if (s.topic.empty() || s.topic.size() > 160) return pre + (s.http ? "URL" : "Thema") + " fehlt (max. 160 Zeichen)";
    if (s.http) {
      if (s.topic.rfind("http://", 0) != 0) return pre + "URL muss mit http:// beginnen (lokales Geraet)";
      if (s.kind == ExtKind::Trigger) return pre + "Ausloeser nur ueber MQTT";
      if (s.intervalSec < 10 || s.intervalSec > 3600) return pre + "Abfrageintervall 10..3600 s";
    } else if (s.topic.find_first_of("#+") != std::string::npos) {
      return pre + "Thema ohne Platzhalter (# +)";
    }
    if (s.path.size() > 80) return pre + "Pfad zu lang";
    if (s.kind == ExtKind::Humidity || s.kind == ExtKind::Battery) {
      if (s.sensor.empty()) return pre + "Zugehoerige Temperaturquelle angeben";
    } else {
      s.sensor.clear();
    }
    n.push_back(std::move(s));
  }
  if (n.size() > kMax) return "Max. " + std::to_string(kMax) + " externe Quellen";
  for (auto& s : n) {
    if (s.sensor.empty()) continue;
    auto it = std::find_if(n.begin(), n.end(), [&](const ExtSource& o) { return o.name == s.sensor; });
    if (it == n.end() || it->kind != ExtKind::Temperature)
      return "Externe Quelle '" + s.name + "': '" + s.sensor + "' ist keine Temperaturquelle";
  }
  // keep runtime state of sources that still exist
  std::vector<ExtStatus> st(n.size());
  for (size_t i = 0; i < n.size(); ++i)
    for (size_t j = 0; j < list.size() && j < status_.size(); ++j)
      if (list[j].name == n[i].name && list[j].topic == n[i].topic) st[i] = status_[j];
  list = std::move(n);
  status_ = std::move(st);
  return std::string();
}

void ExternalSources::toJson(JsonArray a) const {
  for (auto& s : list) {
    JsonObject x = a.add<JsonObject>();
    x["name"] = s.name;
    x["source"] = s.http ? "http" : "mqtt";
    x["topic"] = s.topic;
    x["kind"] = extKindName(s.kind);
    x["path"] = s.path;
    if (!s.sensor.empty()) x["sensor"] = s.sensor;
    if (s.http) x["intervalSec"] = s.intervalSec;
  }
}

void ExternalSources::statusJson(JsonArray a, Millis now) const {
  for (size_t i = 0; i < list.size(); ++i) {
    const ExtStatus& st = i < status_.size() ? status_[i] : ExtStatus();
    JsonObject x = a.add<JsonObject>();
    x["name"] = list[i].name;
    x["count"] = st.count;
    if (st.seen) {
      x["last"] = st.last;
      x["ageSec"] = elapsedMs(now, st.lastMs) / 1000u;
    }
    if (!st.error.empty()) x["error"] = st.error;
    if (list[i].kind == ExtKind::Temperature) x["sensor"] = sensorId(list[i].name);
  }
}

std::vector<std::string> ExternalSources::mqttTopics() const {
  std::vector<std::string> t;
  for (auto& s : list)
    if (!s.http && std::find(t.begin(), t.end(), s.topic) == t.end()) t.push_back(s.topic);
  return t;
}

// ------------------------------------------------------------ values

bool ExternalSources::extract(const std::string& payload, const std::string& path, std::string& text, bool& isNum,
                              float& num) {
  isNum = false;
  if (path.empty()) {
    text = trim(payload).substr(0, 70);
    isNum = parseNumber(payload, num);
    return true;
  }
  JsonDocument d;
  if (deserializeJson(d, payload)) return false;
  JsonVariantConst v = d.as<JsonVariantConst>();
  size_t pos = 0;
  while (pos <= path.size()) {
    size_t dot = path.find('.', pos);
    std::string seg = path.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
    if (seg.empty()) return false;
    if (v.is<JsonArrayConst>()) {
      char* end = nullptr;
      long idx = std::strtol(seg.c_str(), &end, 10);
      if (*end || idx < 0) return false;
      v = v[static_cast<size_t>(idx)];
    } else if (v.is<JsonObjectConst>()) {
      v = v[seg];
    } else {
      return false;
    }
    if (v.isNull()) return false;
    if (dot == std::string::npos) break;
    pos = dot + 1;
  }
  if (v.is<bool>()) {
    text = v.as<bool>() ? "true" : "false";
    num = v.as<bool>() ? 1.f : 0.f;
    isNum = true;
  } else if (v.is<float>()) {
    num = v.as<float>();
    isNum = std::isfinite(num);
    char b[24];
    std::snprintf(b, sizeof(b), "%.2f", static_cast<double>(num));
    text = b;
  } else if (v.is<const char*>()) {
    text = std::string(v.as<const char*>()).substr(0, 70);
    isNum = parseNumber(text, num);
  } else {
    serializeJson(v, text);
    text = text.substr(0, 70);
  }
  return true;
}

void ExternalSources::apply(HeatCoreSystem& sys, size_t idx, const std::string& payload, Millis now) {
  if (status_.size() < list.size()) status_.resize(list.size());
  const ExtSource& s = list[idx];
  ExtStatus& st = status_[idx];
  std::string text;
  bool isNum = false;
  float v = 0.f;
  if (!extract(payload, s.path, text, isNum, v)) {
    // a shared topic (e.g. Shelly events/rpc) carries other components too
    if (!st.seen) st.error = "Pfad '" + s.path + "' nicht in der Nachricht";
    return;
  }
  st.seen = true;
  st.last = text;
  st.lastMs = now;
  ++st.count;
  st.error.clear();
  auto needNum = [&](float lo, float hi) {
    if (!isNum) st.error = "keine Zahl: " + text.substr(0, 30);
    else if (v < lo || v > hi) st.error = "unplausibel: " + text;
    return st.error.empty();
  };
  switch (s.kind) {
    case ExtKind::Temperature:
      if (needNum(-50.f, 120.f))
        sys.sensors.updateExtended(sensorId(s.name), v, st.hasHum, st.hum, st.battery, 100, now);
      break;
    case ExtKind::Humidity:
    case ExtKind::Battery: {
      bool hum = s.kind == ExtKind::Humidity;
      if (!needNum(0.f, hum ? 100.f : 100.f)) break;
      auto it = std::find_if(list.begin(), list.end(), [&](const ExtSource& o) { return o.name == s.sensor; });
      if (it == list.end()) break;
      ExtStatus& tst = status_[static_cast<size_t>(it - list.begin())];
      int8_t batt = static_cast<int8_t>(v >= 20.f ? 1 : 0);  // battery percent -> ok / low
      if (hum) {
        tst.hasHum = true;
        tst.hum = v;
      } else {
        tst.battery = batt;
      }
      if (Sensor* sen = sys.sensors.find(sensorId(s.sensor))) {
        if (hum) {
          sen->hasHumidity = true;
          sen->humidity = v;
        } else {
          sen->batteryOk = batt;
        }
      }
      break;
    }
    case ExtKind::Trigger: sys.mqttInbound(s.name, text); break;
  }
}

bool ExternalSources::ingestMqtt(HeatCoreSystem& sys, const std::string& topic, const std::string& payload, Millis now) {
  bool any = false;
  for (size_t i = 0; i < list.size(); ++i) {
    if (list[i].http || list[i].topic != topic) continue;
    apply(sys, i, payload, now);
    any = true;
  }
  return any;
}

std::vector<size_t> ExternalSources::dueHttp(Millis now) {
  if (status_.size() < list.size()) status_.resize(list.size());
  std::vector<size_t> due;
  for (size_t i = 0; i < list.size(); ++i) {
    if (!list[i].http) continue;
    ExtStatus& st = status_[i];
    if (!st.polled || elapsedMs(now, st.lastPollMs) >= list[i].intervalSec * 1000u) {
      st.polled = true;
      st.lastPollMs = now;
      due.push_back(i);
    }
  }
  return due;
}

void ExternalSources::ingestHttp(HeatCoreSystem& sys, size_t idx, int httpStatus, const std::string& body, Millis now) {
  if (idx >= list.size()) return;
  if (status_.size() < list.size()) status_.resize(list.size());
  if (httpStatus != 200) {
    status_[idx].error = httpStatus <= 0 ? "keine Antwort" : "HTTP " + std::to_string(httpStatus);
    return;
  }
  apply(sys, idx, body, now);
}

}  // namespace hc
