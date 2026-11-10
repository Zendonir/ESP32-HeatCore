#include "hc/ems/EmsEsp.h"

#include <cstring>

namespace hc {

namespace {

bool eq(const char* a, const char* b) {  // ASCII case-insensitive
  if (!a || !b) return false;
  for (; *a && *b; ++a, ++b) {
    char x = *a >= 'A' && *a <= 'Z' ? static_cast<char>(*a + 32) : *a;
    char y = *b >= 'A' && *b <= 'Z' ? static_cast<char>(*b + 32) : *b;
    if (x != y) return false;
  }
  return *a == *b;
}

float num(JsonVariantConst v) {
  if (v.is<float>() || v.is<int>()) return v.as<float>();
  return NAN;
}

std::string text(JsonVariantConst v) {
  if (v.is<const char*>()) return v.as<const char*>();
  return std::string();
}

bool validTopicPart(const std::string& s) {
  if (s.empty() || s.size() > 60) return false;
  for (char c : s)
    if (c == '+' || c == '#' || c == ' ' || c == '"') return false;
  return s.front() != '/' && s.back() != '/';
}

}  // namespace

const char* emsModeKey(EmsMode m) {
  switch (m) {
    case EmsMode::Night: return "night";
    case EmsMode::Day: return "day";
    case EmsMode::Auto: return "auto";
    default: return "";
  }
}

const char* emsReduceKey(EmsReduce r) {
  switch (r) {
    case EmsReduce::NoFrost: return "nofrost";
    case EmsReduce::Reduce: return "reduce";
    case EmsReduce::Room: return "room";
    case EmsReduce::Outdoor: return "outdoor";
    default: return "";
  }
}

const char* emsModeText(EmsMode m) {
  switch (m) {
    case EmsMode::Night: return "Nacht";
    case EmsMode::Day: return "Tag";
    case EmsMode::Auto: return "Automatik";
    default: return "unbekannt";
  }
}

const char* emsReduceText(EmsReduce r) {
  switch (r) {
    case EmsReduce::NoFrost: return "Abschaltbetrieb (Frostschutz)";
    case EmsReduce::Reduce: return "reduziert";
    case EmsReduce::Room: return "Raumhaltebetrieb";
    case EmsReduce::Outdoor: return "Aussenhaltebetrieb";
    default: return "unbekannt";
  }
}

EmsMode emsModeParse(JsonVariantConst v) {
  if (v.is<int>()) {  // EMS-ESP "enum as index": RC35 night, day, auto
    int i = v.as<int>();
    return i >= 0 && i <= 2 ? static_cast<EmsMode>(i) : EmsMode::Unknown;
  }
  const char* s = v.as<const char*>();
  if (!s) return EmsMode::Unknown;
  if (eq(s, "night") || eq(s, "nacht")) return EmsMode::Night;
  if (eq(s, "day") || eq(s, "tag")) return EmsMode::Day;
  if (eq(s, "auto") || eq(s, "automatic") || eq(s, "automatik")) return EmsMode::Auto;
  return EmsMode::Unknown;
}

EmsReduce emsReduceParse(JsonVariantConst v) {
  if (v.is<int>()) {
    int i = v.as<int>();
    return i >= 0 && i <= 3 ? static_cast<EmsReduce>(i) : EmsReduce::Unknown;
  }
  return emsReduceParse(v.as<const char*>());
}

EmsReduce emsReduceParse(const char* s) {
  if (!s) return EmsReduce::Unknown;
  if (eq(s, "nofrost") || eq(s, "frostschutz") || eq(s, "abschalt") || eq(s, "abschaltbetrieb"))
    return EmsReduce::NoFrost;
  if (eq(s, "reduce") || eq(s, "reduziert")) return EmsReduce::Reduce;
  if (eq(s, "room") || eq(s, "raum")) return EmsReduce::Room;
  if (eq(s, "outdoor") || eq(s, "außen") || eq(s, "aussen")) return EmsReduce::Outdoor;
  return EmsReduce::Unknown;
}

int emsBoolParse(JsonVariantConst v) {
  if (v.is<bool>()) return v.as<bool>() ? 1 : 0;
  if (v.is<int>()) return v.as<int>() ? 1 : 0;
  const char* s = v.as<const char*>();
  if (!s) return -1;
  if (eq(s, "on") || eq(s, "an") || eq(s, "true") || eq(s, "ein") || eq(s, "1")) return 1;
  if (eq(s, "off") || eq(s, "aus") || eq(s, "false") || eq(s, "0")) return 0;
  return -1;
}

std::vector<std::string> EmsEsp::topics() const {
  if (!cfg.enabled) return {};
  return {cfg.base + "/boiler_data", cfg.base + "/thermostat_data", cfg.base + "/mixer_data", cfg.base + "/status"};
}

bool EmsEsp::thermostatFresh(Millis now) const { return st.thermoMs && st.hcSeen && elapsed(now, st.thermoMs) < kFreshMs; }
bool EmsEsp::boilerFresh(Millis now) const { return st.boilerMs && elapsed(now, st.boilerMs) < kFreshMs; }
bool EmsEsp::mixerFresh(Millis now) const { return st.mixerMs && elapsed(now, st.mixerMs) < kFreshMs; }

std::string EmsEsp::commandTopic(const char* cmd) const { return cfg.base + "/thermostat/" + hcKey() + "/" + cmd; }

bool EmsEsp::ingest(const std::string& topic, const std::string& payload, Millis now) {
  if (!cfg.enabled) return false;
  const std::string b = cfg.base + "/";
  if (topic.compare(0, b.size(), b) != 0) return false;
  const std::string sub = topic.substr(b.size());
  if (sub == "status") {
    st.online = payload == "online" ? 1 : payload == "offline" ? 0 : st.online;
    return true;
  }
  int which = sub == "boiler_data" ? 0 : sub == "thermostat_data" ? 1 : sub == "mixer_data" ? 2 : -1;
  if (which < 0) return false;
  // keep only what is used: thermostat_data is ~3 KB every 10 s
  JsonDocument filter;
  std::string hk = hcKey();
  if (which == 0) {
    for (const char* k : {"outdoortemp", "curflowtemp", "rettemp", "selflowtemp", "syspress", "curburnpow",
                          "heatingactive", "burngas", "servicecode", "lastcode", "tapwateractive", "wwcurtemp"})
      filter[k] = true;
    filter["dhw"]["curtemp"] = true;
  } else if (which == 1) {
    for (const char* k : {"mode", "modetype", "reducemode", "seltemp", "targetflowtemp", "summermode", "heatingtype"})
      filter[hk][k] = true;
    filter["dampedoutdoortemp"] = true;
    filter["lastcode"] = true;
  } else {
    for (const char* k : {"flowtemphc", "valvestatus", "pumpstatus"}) filter[hk][k] = true;
  }
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (e || !doc.is<JsonObjectConst>()) {
    st.error = std::string(sub) + ": kein JSON (" + e.c_str() + ") - EMS-ESP-Einstellung MQTT-Format \"verschachtelt\"?";
    return true;
  }
  ++st.messages;
  JsonObjectConst o = doc.as<JsonObjectConst>();
  if (which == 0) parseBoiler(o, now);
  else if (which == 1) parseThermostat(o, now);
  else parseMixer(o, now);
  return true;
}

void EmsEsp::parseBoiler(JsonObjectConst o, Millis now) {
  st.boilerMs = now ? now : 1;
  st.outdoor = num(o["outdoortemp"]);
  st.boilerFlow = num(o["curflowtemp"]);
  st.boilerReturn = num(o["rettemp"]);
  st.boilerSetFlow = num(o["selflowtemp"]);
  st.pressure = num(o["syspress"]);
  st.burnPower = num(o["curburnpow"]);
  st.heatingActive = static_cast<int8_t>(emsBoolParse(o["heatingactive"]));
  st.burner = static_cast<int8_t>(emsBoolParse(o["burngas"]));
  st.dhwActive = static_cast<int8_t>(emsBoolParse(o["tapwateractive"]));
  st.dhwTemp = num(o["dhw"]["curtemp"]);
  if (std::isnan(st.dhwTemp)) st.dhwTemp = num(o["wwcurtemp"]);  // EMS-ESP before 3.6
  st.serviceCode = text(o["servicecode"]);
  st.boilerLastCode = text(o["lastcode"]);
}

void EmsEsp::parseThermostat(JsonObjectConst o, Millis now) {
  st.dampedOutdoor = num(o["dampedoutdoortemp"]);
  st.thermoLastCode = text(o["lastcode"]);
  JsonObjectConst h = o[hcKey()];
  if (h.isNull()) {
    st.error = "Thermostat meldet keinen Heizkreis " + std::to_string(cfg.hc);
    st.hcSeen = false;
    st.thermoMs = now ? now : 1;
    return;
  }
  st.thermoMs = now ? now : 1;
  st.hcSeen = true;
  st.mode = emsModeParse(h["mode"]);
  st.modeType = text(h["modetype"]);
  st.reduce = emsReduceParse(h["reducemode"]);
  st.reduceRaw = text(h["reducemode"]);
  st.selTemp = num(h["seltemp"]);
  st.targetFlow = num(h["targetflowtemp"]);
  st.summerMode = text(h["summermode"]);
  st.heatingType = text(h["heatingtype"]);
  if (st.error.rfind("Thermostat", 0) == 0) st.error.clear();
}

void EmsEsp::parseMixer(JsonObjectConst o, Millis now) {
  JsonObjectConst h = o[hcKey()];
  if (h.isNull()) return;  // the mixer serves another circuit
  st.mixerMs = now ? now : 1;
  st.hcFlow = num(h["flowtemphc"]);
  st.valve = num(h["valvestatus"]);
  st.pump = static_cast<int8_t>(emsBoolParse(h["pumpstatus"]));
}

void EmsEsp::toJson(JsonObject o) const {
  o["enabled"] = cfg.enabled;
  o["base"] = cfg.base;
  o["hc"] = cfg.hc;
  o["sensors"] = cfg.sensors;
  o["savedMode"] = cfg.savedMode;
  o["savedReduce"] = cfg.savedReduce;
}

std::string EmsEsp::fromJson(JsonObjectConst o, bool keepSaved) {
  EmsEspConfig n;
  n.enabled = o["enabled"] | false;
  n.base = o["base"] | "ems-esp";
  n.hc = static_cast<uint8_t>(o["hc"] | 2);
  n.sensors = o["sensors"] | true;
  n.savedMode = keepSaved ? cfg.savedMode : std::string(o["savedMode"] | "");
  n.savedReduce = keepSaved ? cfg.savedReduce : std::string(o["savedReduce"] | "");
  if (!validTopicPart(n.base)) return "Basisthema: 1..60 Zeichen, ohne Leerzeichen, + und #";
  if (n.hc < 1 || n.hc > 4) return "Heizkreis 1..4";
  if (!n.savedReduce.empty() && emsReduceParse(n.savedReduce.c_str()) == EmsReduce::Unknown)
    n.savedReduce.clear();
  if (n.savedMode != "day" && n.savedMode != "night" && n.savedMode != "auto") n.savedMode.clear();
  bool changed = n.enabled != cfg.enabled || n.base != cfg.base || n.hc != cfg.hc;
  cfg = std::move(n);
  if (changed) {
    clearState();
    ++revision;
  }
  return std::string();
}

void EmsEsp::statusJson(JsonObject o, Millis now) const {
  o["enabled"] = cfg.enabled;
  if (st.online >= 0) o["online"] = st.online == 1;
  o["messages"] = st.messages;
  if (!st.error.empty()) o["error"] = st.error;
  auto age = [&](Millis t) { return t ? static_cast<int32_t>(elapsed(now, t) / 1000u) : -1; };
  auto put = [](JsonObject x, const char* k, float v) {
    if (!std::isnan(v)) x[k] = std::round(v * 10.f) / 10.f;
  };
  auto putB = [](JsonObject x, const char* k, int8_t v) {
    if (v >= 0) x[k] = v == 1;
  };
  JsonObject b = o["boiler"].to<JsonObject>();
  b["ageSec"] = age(st.boilerMs);
  b["fresh"] = boilerFresh(now);
  put(b, "outdoor", st.outdoor);
  put(b, "flow", st.boilerFlow);
  put(b, "return", st.boilerReturn);
  put(b, "setFlow", st.boilerSetFlow);
  put(b, "pressure", st.pressure);
  put(b, "dhw", st.dhwTemp);
  put(b, "burnPower", st.burnPower);
  putB(b, "heatingActive", st.heatingActive);
  putB(b, "burner", st.burner);
  putB(b, "dhwActive", st.dhwActive);
  if (!st.serviceCode.empty()) b["serviceCode"] = st.serviceCode;
  if (!st.boilerLastCode.empty()) b["lastCode"] = st.boilerLastCode;
  JsonObject t = o["thermostat"].to<JsonObject>();
  t["ageSec"] = age(st.thermoMs);
  t["fresh"] = thermostatFresh(now);
  t["hc"] = cfg.hc;
  if (st.hcSeen) {
    t["mode"] = emsModeKey(st.mode);
    t["modeText"] = emsModeText(st.mode);
    t["modeType"] = st.modeType;
    t["reduce"] = emsReduceKey(st.reduce);
    t["reduceText"] = st.reduce == EmsReduce::Unknown && !st.reduceRaw.empty() ? st.reduceRaw : emsReduceText(st.reduce);
    put(t, "selTemp", st.selTemp);
    put(t, "targetFlow", st.targetFlow);
    t["summerMode"] = st.summerMode;
    t["heatingType"] = st.heatingType;
  }
  put(t, "dampedOutdoor", st.dampedOutdoor);
  if (!st.thermoLastCode.empty()) t["lastCode"] = st.thermoLastCode;
  JsonObject m = o["mixer"].to<JsonObject>();
  m["ageSec"] = age(st.mixerMs);
  m["fresh"] = mixerFresh(now);
  put(m, "flow", st.hcFlow);
  put(m, "valve", st.valve);
  putB(m, "pump", st.pump);
  o["takenOver"] = !cfg.savedReduce.empty();
  if (!cfg.savedReduce.empty()) {
    o["savedMode"] = cfg.savedMode;
    o["savedReduce"] = cfg.savedReduce;
  }
}

}  // namespace hc
