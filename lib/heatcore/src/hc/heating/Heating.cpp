#include "hc/heating/Heating.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hc {

namespace {
const char* const kRoomModes[] = {"auto", "comfort", "eco", "frost", "off"};
const char* const kGlobalModes[] = {"normal", "eco", "away", "vacation"};

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

const char* roomModeName(RoomMode m) { return kRoomModes[static_cast<int>(m)]; }
bool roomModeFromName(const char* s, RoomMode& out) {
  for (int i = 0; i < 5; ++i)
    if (s && !std::strcmp(s, kRoomModes[i])) {
      out = static_cast<RoomMode>(i);
      return true;
    }
  return false;
}
const char* globalModeName(GlobalMode m) { return kGlobalModes[static_cast<int>(m)]; }
bool globalModeFromName(const char* s, GlobalMode& out) {
  for (int i = 0; i < 4; ++i)
    if (s && !std::strcmp(s, kGlobalModes[i])) {
      out = static_cast<GlobalMode>(i);
      return true;
    }
  return false;
}

std::string formatHHMM(uint16_t m) {
  char b[8];
  std::snprintf(b, sizeof(b), "%02u:%02u", static_cast<unsigned>(m / 60) % 100, static_cast<unsigned>(m % 60));
  return b;
}

bool parseHHMM(const char* s, uint16_t& out) {
  unsigned h = 0, m = 0;
  if (!s || std::sscanf(s, "%u:%u", &h, &m) != 2 || h > 24 || m > 59 || (h == 24 && m != 0)) return false;
  out = static_cast<uint16_t>(h * 60 + m);
  return true;
}

HeatingManager::HeatingManager(EventBus& bus, const IClock& clock) : bus_(bus), clock_(clock) {
  for (int i = 0; i < kNumOutputs; ++i) {
    circuits[i].index = i + 1;
    circuits[i].name = "Heizkreis " + std::to_string(i + 1);
  }
}

Room* HeatingManager::room(const std::string& id) {
  for (auto& r : rooms)
    if (r.id == id) return &r;
  return nullptr;
}
const Room* HeatingManager::room(const std::string& id) const {
  for (auto& r : rooms)
    if (r.id == id) return &r;
  return nullptr;
}

std::vector<Room*> HeatingManager::roomsInGroup(const std::string& group) {
  std::vector<Room*> v;
  for (auto& r : rooms)
    if (std::find(r.groups.begin(), r.groups.end(), group) != r.groups.end()) v.push_back(&r);
  return v;
}

std::vector<std::string> HeatingManager::groups() const {
  std::vector<std::string> g;
  for (auto& r : rooms)
    for (auto& x : r.groups)
      if (std::find(g.begin(), g.end(), x) == g.end()) g.push_back(x);
  return g;
}

void HeatingManager::emitRoom(EventType t, const Room& r, float value, Millis now, uint8_t depth, uint16_t ruleId,
                              EventSource src) {
  Event e(t, src);
  e.ts = now;
  e.depth = depth;
  e.ruleId = ruleId;
  e.setRef(r.id.c_str());
  e.value = value;
  bus_.post(e);
}

bool HeatingManager::setManualSetpoint(Room& r, float sp, float minSp, float maxSp, Millis now, uint8_t depth,
                                       uint16_t ruleId, EventSource src) {
  sp = clampf(std::round(sp * 10.f) / 10.f, minSp, maxSp);
  bool changed = !r.manualActive || std::fabs(r.manualSetpoint - sp) > 0.01f;
  r.manualActive = true;
  r.manualSetpoint = sp;
  r.manualUntil = now + r.manualOverrideMin * 60u * 1000u;
  bool comfort = false;
  r.manualSlot = scheduleSlot(r, clock_.local(), &comfort);
  if (changed) emitRoom(EventType::RoomSetpointChanged, r, sp, now, depth, ruleId, src);
  return changed;
}

bool HeatingManager::adjustSetpoint(Room& r, float delta, float minSp, float maxSp, Millis now, uint8_t depth,
                                    uint16_t ruleId, EventSource src) {
  float base = r.manualActive ? r.manualSetpoint : (r.setpoint > 0.f ? r.setpoint : r.comfort);
  return setManualSetpoint(r, base + delta, minSp, maxSp, now, depth, ruleId, src);
}

bool HeatingManager::setRoomMode(Room& r, RoomMode m, uint8_t depth, uint16_t ruleId, EventSource src) {
  if (r.mode == m && !r.manualActive) return false;
  r.mode = m;
  r.manualActive = false;  // a deliberate mode change ends a manual override
  ++revision_;
  emitRoom(EventType::RoomModeChanged, r, static_cast<float>(m), clock_.nowMs(), depth, ruleId, src);
  return true;
}

void HeatingManager::setGlobalMode(GlobalMode m, uint32_t durationSec, Millis now, uint8_t depth, uint16_t ruleId,
                                   EventSource src) {
  bool changed = globalMode != m;
  globalMode = m;
  globalModeTimed = durationSec > 0 && m != GlobalMode::Normal;
  globalModeUntilMs = now + durationSec * 1000u;
  int64_t ep = clock_.epoch();
  globalModeUntilEpoch = globalModeTimed && ep ? ep + durationSec : 0;
  ++revision_;
  if (changed) {
    Event e(EventType::GlobalModeChanged, src);
    e.ts = now;
    e.depth = depth;
    e.ruleId = ruleId;
    e.setRef(globalModeName(m));
    e.value = static_cast<float>(m);
    bus_.post(e);
  }
}

void HeatingManager::setWindow(Room& r, const std::string& source, bool open, Millis now, uint8_t depth,
                               uint16_t ruleId, EventSource src) {
  bool was = r.windowOpen();
  auto it = std::find(r.windowSources.begin(), r.windowSources.end(), source);
  if (open && it == r.windowSources.end()) r.windowSources.push_back(source);
  if (!open && it != r.windowSources.end()) r.windowSources.erase(it);
  if (was != r.windowOpen()) emitRoom(EventType::RoomWindowChanged, r, r.windowOpen() ? 1.f : 0.f, now, depth, ruleId, src);
}

void HeatingManager::setPresence(Room* r, bool present, Millis now, uint8_t depth, uint16_t ruleId, EventSource src) {
  if (r) {
    int8_t p = present ? 1 : 0;
    if (r->presence == p) return;
    r->presence = p;
    emitRoom(EventType::RoomPresenceChanged, *r, p, now, depth, ruleId, src);
    return;
  }
  for (auto& x : rooms) setPresence(&x, present, now, depth, ruleId, src);
}

bool HeatingManager::homePresent() const {
  bool anyKnown = false;
  for (auto& r : rooms) {
    if (r.presence == 1) return true;
    if (r.presence == 0) anyKnown = true;
  }
  return !anyKnown;  // unknown counts as present
}

void HeatingManager::setExternalDemand(Room& r, const std::string& source, bool on, uint32_t durationSec, Millis now) {
  auto it = std::find(r.demandSources.begin(), r.demandSources.end(), source);
  if (on && it == r.demandSources.end()) r.demandSources.push_back(source);
  if (!on && it != r.demandSources.end()) r.demandSources.erase(it);
  r.externalDemand = !r.demandSources.empty();
  // A heat request always expires: lost "off" telegrams must not keep heating.
  uint32_t d = durationSec ? durationSec : 3600;
  if (d > 4 * 3600) d = 4 * 3600;
  r.externalDemandUntil = now + d * 1000u;
}

int HeatingManager::scheduleSlot(const Room& r, const LocalTime& lt, bool* comfort) {
  *comfort = false;
  if (!lt.valid) return -1;
  int day = lt.weekday;
  int m = lt.minuteOfDay();
  const auto& periods = r.schedule[static_cast<size_t>(day)];
  for (size_t i = 0; i < periods.size(); ++i) {
    if (m >= periods[i].start && m < periods[i].end) {
      *comfort = true;
      return day * 16 + static_cast<int>(i);
    }
  }
  // outside comfort periods: slot identifies the gap
  int gap = 0;
  for (auto& p : periods)
    if (m >= p.end) ++gap;
  return day * 16 + 8 + gap;
}

float HeatingManager::computeSetpoint(Room& r, const LocalTime& lt, float minSp, float maxSp, Millis now) {
  bool comfort = false;
  int slot = scheduleSlot(r, lt, &comfort);
  if (r.manualActive) {
    bool expired = r.manualOverrideMin > 0 && reached(now, r.manualUntil);
    bool slotChanged = slot >= 0 && r.manualSlot >= 0 && slot != r.manualSlot && r.mode == RoomMode::Auto;
    if (expired || slotChanged) {
      r.manualActive = false;
      emitRoom(EventType::RoomSetpointChanged, r, 0.f, now, 0, 0, EventSource::Schedule);
    }
  }
  if (r.externalDemand && reached(now, r.externalDemandUntil)) {
    r.externalDemand = false;
    r.demandSources.clear();
  }
  float sp;
  if (r.mode == RoomMode::Off) {
    sp = r.frost;
    r.reason = "aus (Frostschutz)";
  } else if (r.windowOpen()) {
    sp = r.frost;
    r.reason = "Fenster offen";
  } else if (globalMode == GlobalMode::Vacation) {
    sp = r.frost;
    r.reason = "Urlaub";
  } else if (r.manualActive) {
    sp = r.manualSetpoint;
    r.reason = "manuell";
  } else if (globalMode == GlobalMode::Away || globalMode == GlobalMode::Eco) {
    sp = r.eco;
    r.reason = globalMode == GlobalMode::Away ? "abwesend" : "Absenkbetrieb";
  } else if (r.mode == RoomMode::Comfort) {
    sp = r.comfort;
    r.reason = "Komfort";
  } else if (r.mode == RoomMode::Eco) {
    sp = r.eco;
    r.reason = "Absenkung";
  } else if (r.mode == RoomMode::Frost) {
    sp = r.frost;
    r.reason = "Frostschutz";
  } else if (r.presence == 0) {
    sp = r.eco;
    r.reason = "niemand anwesend";
  } else if (slot < 0) {
    sp = r.comfort;
    r.reason = "Komfort (keine Uhrzeit)";
  } else {
    sp = comfort ? r.comfort : r.eco;
    r.reason = comfort ? "Zeitplan Komfort" : "Zeitplan Absenkung";
  }
  // Frost protection may lie below the user range but never below 5 C.
  float lo = sp == r.frost ? 5.f : minSp;
  r.setpoint = clampf(sp, lo, maxSp);
  return r.setpoint;
}

void HeatingManager::updateDemand(Room& r, const ModelAdvice& adv, SensorFailMode failMode, uint8_t dutyPct,
                                  uint16_t cycleMin, Millis now) {
  bool d = r.demand;
  r.assistOnShift = r.assistOffShift = 0.f;
  if (r.overTemp) {
    d = false;
  } else if (!r.hasTemp) {
    r.sensorFail = true;
    if (failMode == SensorFailMode::Duty && r.mode != RoomMode::Off && !r.windowOpen() && dutyPct > 0) {
      uint32_t cycle = static_cast<uint32_t>(cycleMin ? cycleMin : 20) * 60u * 1000u;
      uint32_t phase = now % cycle;
      d = phase < cycle / 100u * (dutyPct > 100 ? 100 : dutyPct);
    } else {
      d = false;
    }
  } else {
    r.sensorFail = false;
    float h = clampf(r.hysteresis, 0.1f, 3.f);
    float lower = r.setpoint - h / 2.f;
    float upper = r.setpoint + h / 2.f;
    if (adv.valid && r.modelAssist) {
      r.assistOnShift = clampf(adv.startEarlyK, 0.f, 0.5f);
      r.assistOffShift = clampf(adv.stopEarlyK, 0.f, 0.5f);
      lower += r.assistOnShift;
      upper -= r.assistOffShift;
      if (upper < lower + 0.1f) upper = lower + 0.1f;
    }
    if (d && r.temperature >= upper) d = false;
    else if (!d && r.temperature <= lower) d = true;
    // External heat request (RF thermostat etc.) is bounded by setpoint + 1 K.
    if (r.externalDemand && !r.windowOpen() && r.temperature < r.setpoint + 1.0f) d = true;
  }
  if (d != r.demand) {
    r.demand = d;
    r.demandChangedMs = now;
    emitRoom(EventType::RoomHeatingChanged, r, d ? 1.f : 0.f, now, 0, 0, EventSource::System);
  }
}

void HeatingManager::updateCircuits(Millis now) {
  for (auto& c : circuits) {
    const Room* r = c.room.empty() ? nullptr : room(c.room);
    c.request = c.enabled && r && r->demand;
    if (c.request == c.open) continue;
    uint32_t minT = (c.open ? c.minOnSec : c.minOffSec) * 1000u;
    if (c.switched && elapsed(now, c.lastSwitchMs) < minT) continue;
    c.open = c.request;
    c.lastSwitchMs = now;
    c.switched = true;
    if (c.open) c.openSinceMs = now;
    Event e(EventType::ValveChanged, EventSource::System);
    e.ts = now;
    e.a = c.index;
    e.setRef(c.room.c_str());
    e.value = c.open ? 1.f : 0.f;
    bus_.post(e);
  }
}

std::string HeatingManager::validateRoomId(const std::string& id) {
  if (id.empty() || id.size() > 15) return "Raum-ID 1..15 Zeichen";
  for (char c : id)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
      return "Raum-ID nur a-z, 0-9, _ und -";
  return std::string();
}

void HeatingManager::toJson(JsonObject o, bool runtime) const {
  Millis now = clock_.nowMs();
  o["globalMode"] = globalModeName(globalMode);
  o["globalModeUntilEpoch"] = globalModeUntilEpoch;
  if (runtime && globalModeTimed)
    o["globalModeRemainingSec"] = reached(now, globalModeUntilMs) ? 0u : (globalModeUntilMs - now) / 1000u;
  JsonArray ra = o["rooms"].to<JsonArray>();
  for (auto& r : rooms) {
    JsonObject x = ra.add<JsonObject>();
    x["id"] = r.id;
    x["name"] = r.name;
    x["comfort"] = r.comfort;
    x["eco"] = r.eco;
    x["frost"] = r.frost;
    x["mode"] = roomModeName(r.mode);
    x["hysteresis"] = r.hysteresis;
    JsonArray g = x["groups"].to<JsonArray>();
    for (auto& s : r.groups) g.add(s);
    JsonArray sch = x["schedule"].to<JsonArray>();
    for (auto& day : r.schedule) {
      JsonArray d = sch.add<JsonArray>();
      for (auto& p : day) d.add(formatHHMM(p.start) + "-" + formatHHMM(p.end));
    }
    x["modelAssist"] = r.modelAssist;
    x["manualOverrideMin"] = r.manualOverrideMin;
    x["showOnDisplay"] = r.showOnDisplay;
    if (!runtime) continue;
    x["windowOpen"] = r.windowOpen();
    JsonArray ws = x["windowSources"].to<JsonArray>();
    for (auto& s : r.windowSources) ws.add(s);
    if (r.presence >= 0) x["presence"] = r.presence == 1;
    x["manualActive"] = r.manualActive;
    if (r.manualActive) {
      x["manualSetpoint"] = r.manualSetpoint;
      if (r.manualOverrideMin)
        x["manualRemainingMin"] = reached(now, r.manualUntil) ? 0u : (r.manualUntil - now) / 60000u;
    }
    x["externalDemand"] = r.externalDemand;
    x["hasTemp"] = r.hasTemp;
    if (r.hasTemp) x["temperature"] = r.temperature;
    x["tempSensor"] = r.tempSensor;
    x["tempFallback"] = r.tempFallback;
    x["setpoint"] = r.setpoint;
    x["reason"] = r.reason;
    x["demand"] = r.demand;
    x["sensorFail"] = r.sensorFail;
    x["overTemp"] = r.overTemp;
    x["assistOnShift"] = r.assistOnShift;
    x["assistOffShift"] = r.assistOffShift;
  }
  JsonArray ca = o["circuits"].to<JsonArray>();
  for (auto& c : circuits) {
    JsonObject x = ca.add<JsonObject>();
    x["n"] = c.index;
    x["name"] = c.name;
    x["room"] = c.room;
    x["enabled"] = c.enabled;
    x["valveNormallyOpen"] = c.valveNormallyOpen;
    x["minOnSec"] = c.minOnSec;
    x["minOffSec"] = c.minOffSec;
    if (!runtime) continue;
    x["request"] = c.request;
    x["open"] = c.open;
    x["fault"] = c.fault;
    x["faultText"] = c.faultText;
  }
}

std::string HeatingManager::fromJson(JsonObjectConst o) {
  std::vector<Room> list;
  for (JsonObjectConst x : o["rooms"].as<JsonArrayConst>()) {
    Room r;
    r.id = x["id"] | "";
    std::string e = validateRoomId(r.id);
    if (!e.empty()) return e;
    for (auto& other : list)
      if (other.id == r.id) return "Raum-ID doppelt: " + r.id;
    r.name = x["name"] | r.id.c_str();
    r.comfort = x["comfort"] | 21.f;
    r.eco = x["eco"] | 18.f;
    r.frost = x["frost"] | 7.f;
    if (r.frost < 5.f || r.frost > 12.f) return "Frostschutz 5..12 C";
    if (r.eco < r.frost || r.comfort < r.eco) return "Erwartet: Frostschutz <= Absenkung <= Komfort";
    if (!roomModeFromName(x["mode"] | "auto", r.mode)) return "Ungueltige Raumbetriebsart";
    r.hysteresis = x["hysteresis"] | 0.4f;
    if (r.hysteresis < 0.1f || r.hysteresis > 3.f) return "Hysterese 0.1..3 K";
    for (const char* g : x["groups"].as<JsonArrayConst>())
      if (g && *g) r.groups.push_back(g);
    JsonArrayConst sch = x["schedule"];
    if (sch.isNull()) {
      for (int d = 0; d < 7; ++d) {
        if (d < 5) r.schedule[d] = {{6 * 60, 8 * 60}, {16 * 60, 22 * 60}};
        else r.schedule[d] = {{7 * 60, 22 * 60}};
      }
    } else {
      int d = 0;
      for (JsonArrayConst day : sch) {
        if (d >= 7) break;
        for (const char* p : day) {
          std::string s = p ? p : "";
          size_t dash = s.find('-');
          Period per;
          if (dash == std::string::npos || !parseHHMM(s.substr(0, dash).c_str(), per.start) ||
              !parseHHMM(s.substr(dash + 1).c_str(), per.end) || per.end <= per.start)
            return "Ungueltiger Zeitraum: " + s;
          r.schedule[d].push_back(per);
        }
        if (r.schedule[d].size() > 6) return "Max. 6 Komfortzeiten pro Tag";
        std::sort(r.schedule[d].begin(), r.schedule[d].end(),
                  [](const Period& a, const Period& b) { return a.start < b.start; });
        ++d;
      }
    }
    r.modelAssist = x["modelAssist"] | true;
    r.manualOverrideMin = x["manualOverrideMin"] | 180u;
    r.showOnDisplay = x["showOnDisplay"] | true;
    if (Room* old = room(r.id)) {
      // keep runtime state across configuration updates
      r.windowSources = old->windowSources;
      r.presence = old->presence;
      r.manualActive = old->manualActive;
      r.manualSetpoint = old->manualSetpoint;
      r.manualUntil = old->manualUntil;
      r.manualSlot = old->manualSlot;
      r.externalDemand = old->externalDemand;
      r.externalDemandUntil = old->externalDemandUntil;
      r.demandSources = old->demandSources;
      r.demand = old->demand;
      r.demandChangedMs = old->demandChangedMs;
      r.hasTemp = old->hasTemp;
      r.temperature = old->temperature;
      r.setpoint = old->setpoint;
    }
    list.push_back(std::move(r));
  }
  if (list.size() > 16) return "Maximal 16 Raeume";
  for (JsonObjectConst x : o["circuits"].as<JsonArrayConst>()) {
    int n = x["n"] | 0;
    if (n < 1 || n > kNumOutputs) continue;
    Circuit& c = circuits[n - 1];
    c.name = x["name"] | c.name.c_str();
    c.room = x["room"] | "";
    c.enabled = x["enabled"] | true;
    c.valveNormallyOpen = x["valveNormallyOpen"] | false;
    c.minOnSec = x["minOnSec"] | 120;
    c.minOffSec = x["minOffSec"] | 120;
    if (c.minOnSec > 3600 || c.minOffSec > 3600) return "Mindestlaufzeiten max. 3600 s";
  }
  rooms = std::move(list);
  GlobalMode gm = GlobalMode::Normal;
  globalModeFromName(o["globalMode"] | "normal", gm);
  globalMode = gm;
  globalModeUntilEpoch = o["globalModeUntilEpoch"] | static_cast<int64_t>(0);
  globalModeTimed = globalModeUntilEpoch != 0;
  ++revision_;
  return std::string();
}

}  // namespace hc
