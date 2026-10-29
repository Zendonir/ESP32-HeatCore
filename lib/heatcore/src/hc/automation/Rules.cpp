#include "hc/automation/Rules.h"

#include <cstring>

#include "hc/heating/Heating.h"

namespace hc {

namespace {
const char* const kTriggerNames[] = {"input",  "temp_threshold", "temp_change", "room_state",  "valve_state", "time",
                                     "mqtt",   "sensor_fault",   "model_event", "manual",      "global_mode", "alarm"};
static_assert(sizeof(kTriggerNames) / sizeof(kTriggerNames[0]) == static_cast<size_t>(TriggerType::Count), "");
const char* const kConditionNames[] = {"temperature", "setpoint",         "time_range",  "weekday",
                                       "global_mode", "room_mode",        "presence",    "window",
                                       "sensor_available", "input_state", "output_state", "room_group"};
static_assert(sizeof(kConditionNames) / sizeof(kConditionNames[0]) == static_cast<size_t>(ConditionType::Count), "");
const char* const kOps[] = {"<", "<=", "==", ">=", ">"};

uint8_t daysFromJson(JsonVariantConst v) {
  if (v.isNull()) return 0x7F;
  if (v.is<int>()) return static_cast<uint8_t>(v.as<int>() & 0x7F);
  uint8_t m = 0;
  for (int d : v.as<JsonArrayConst>())
    if (d >= 0 && d < 7) m |= static_cast<uint8_t>(1u << d);
  return m;
}
void daysToJson(uint8_t mask, JsonArray a) {
  for (int d = 0; d < 7; ++d)
    if (mask & (1u << d)) a.add(d);
}
}  // namespace

const char* triggerTypeName(TriggerType t) { return kTriggerNames[static_cast<int>(t)]; }
bool triggerTypeFromName(const char* s, TriggerType& out) {
  for (size_t i = 0; i < static_cast<size_t>(TriggerType::Count); ++i)
    if (s && !std::strcmp(s, kTriggerNames[i])) {
      out = static_cast<TriggerType>(i);
      return true;
    }
  return false;
}
const char* conditionTypeName(ConditionType t) { return kConditionNames[static_cast<int>(t)]; }
bool conditionTypeFromName(const char* s, ConditionType& out) {
  for (size_t i = 0; i < static_cast<size_t>(ConditionType::Count); ++i)
    if (s && !std::strcmp(s, kConditionNames[i])) {
      out = static_cast<ConditionType>(i);
      return true;
    }
  return false;
}
const char* compareOpName(CompareOp o) { return kOps[static_cast<int>(o)]; }
bool compareOpFromName(const char* s, CompareOp& out) {
  for (int i = 0; i < 5; ++i)
    if (s && !std::strcmp(s, kOps[i])) {
      out = static_cast<CompareOp>(i);
      return true;
    }
  return false;
}
bool compare(float a, CompareOp op, float b) {
  switch (op) {
    case CompareOp::Lt: return a < b;
    case CompareOp::Le: return a <= b;
    case CompareOp::Eq: return a > b - 0.05f && a < b + 0.05f;
    case CompareOp::Ge: return a >= b;
    case CompareOp::Gt: return a > b;
  }
  return false;
}

void triggerToJson(const Trigger& t, JsonObject o) {
  o["type"] = triggerTypeName(t.type);
  switch (t.type) {
    case TriggerType::Input:
      o["ref"] = t.ref;
      o["gesture"] = gestureName(t.gesture);
      break;
    case TriggerType::TempThreshold:
      o["ref"] = t.ref;
      o["direction"] = t.above ? "above" : "below";
      o["value"] = t.value;
      o["hysteresis"] = t.hysteresis;
      break;
    case TriggerType::TempChange:
      o["ref"] = t.ref;
      o["delta"] = t.value;
      o["windowSec"] = t.windowSec;
      break;
    case TriggerType::RoomState:
    case TriggerType::ModelEvent:
      o["ref"] = t.ref;
      o["state"] = t.state;
      break;
    case TriggerType::ValveState:
      o["circuit"] = t.index;
      o["state"] = t.state;
      break;
    case TriggerType::Time:
      o["time"] = formatHHMM(t.time);
      daysToJson(t.days, o["days"].to<JsonArray>());
      break;
    case TriggerType::Mqtt:
      o["topic"] = t.ref;
      o["payload"] = t.text;
      break;
    case TriggerType::SensorFault:
    case TriggerType::Alarm: o["ref"] = t.ref; break;
    case TriggerType::GlobalMode: o["state"] = t.state; break;
    default: break;
  }
}

bool triggerFromJson(Trigger& t, JsonObjectConst o, std::string& err) {
  t = Trigger{};
  if (!triggerTypeFromName(o["type"] | "", t.type)) {
    err = std::string("unbekannter Trigger: ") + (o["type"] | "");
    return false;
  }
  t.ref = o["ref"] | "";
  t.state = o["state"] | "";
  switch (t.type) {
    case TriggerType::Input: {
      const char* g = o["gesture"] | "any";
      t.gesture = std::strcmp(g, "any") == 0 ? Gesture::None : gestureFromName(g);
      if (t.gesture == Gesture::None && std::strcmp(g, "any") != 0) {
        err = "unbekannte Geste";
        return false;
      }
      break;
    }
    case TriggerType::TempThreshold:
      t.above = std::strcmp(o["direction"] | "above", "below") != 0;
      t.value = o["value"] | 0.f;
      t.hysteresis = o["hysteresis"] | 0.3f;
      break;
    case TriggerType::TempChange:
      t.value = o["delta"] | 0.f;
      t.windowSec = o["windowSec"] | 600u;
      break;
    case TriggerType::ValveState: t.index = o["circuit"] | 0; break;
    case TriggerType::Time:
      if (!parseHHMM(o["time"] | "", t.time)) {
        err = "Zeit im Format HH:MM erwartet";
        return false;
      }
      t.days = daysFromJson(o["days"]);
      break;
    case TriggerType::Mqtt:
      t.ref = o["topic"] | "";
      t.text = o["payload"] | "";
      break;
    default: break;
  }
  return true;
}

void conditionToJson(const Condition& c, JsonObject o) {
  o["type"] = conditionTypeName(c.type);
  if (c.negate) o["negate"] = true;
  switch (c.type) {
    case ConditionType::Temperature:
    case ConditionType::Setpoint:
      o["ref"] = c.ref;
      o["op"] = compareOpName(c.op);
      o["value"] = c.value;
      break;
    case ConditionType::TimeRange:
      o["from"] = formatHHMM(c.from);
      o["to"] = formatHHMM(c.to);
      break;
    case ConditionType::Weekday: daysToJson(c.days, o["days"].to<JsonArray>()); break;
    case ConditionType::GlobalMode: o["mode"] = c.text; break;
    case ConditionType::RoomMode:
      o["ref"] = c.ref;
      o["mode"] = c.text;
      break;
    case ConditionType::Presence:
    case ConditionType::Window:
    case ConditionType::SensorAvailable:
    case ConditionType::InputState:
      o["ref"] = c.ref;
      o["value"] = c.value != 0.f;
      break;
    case ConditionType::OutputState:
      o["output"] = c.index;
      o["value"] = c.value != 0.f;
      break;
    case ConditionType::RoomGroup:
      o["ref"] = c.ref;
      o["state"] = c.text;
      break;
    default: break;
  }
}

bool conditionFromJson(Condition& c, JsonObjectConst o, std::string& err) {
  c = Condition{};
  if (!conditionTypeFromName(o["type"] | "", c.type)) {
    err = std::string("unbekannte Bedingung: ") + (o["type"] | "");
    return false;
  }
  c.negate = o["negate"] | false;
  c.ref = o["ref"] | "";
  switch (c.type) {
    case ConditionType::Temperature:
    case ConditionType::Setpoint:
      if (!compareOpFromName(o["op"] | ">=", c.op)) {
        err = "unbekannter Vergleich";
        return false;
      }
      c.value = o["value"] | 0.f;
      break;
    case ConditionType::TimeRange:
      if (!parseHHMM(o["from"] | "", c.from) || !parseHHMM(o["to"] | "", c.to)) {
        err = "Zeitbereich im Format HH:MM erwartet";
        return false;
      }
      break;
    case ConditionType::Weekday: c.days = daysFromJson(o["days"]); break;
    case ConditionType::GlobalMode:
    case ConditionType::RoomMode: c.text = o["mode"] | ""; break;
    case ConditionType::Presence:
    case ConditionType::Window:
    case ConditionType::SensorAvailable:
    case ConditionType::InputState: c.value = (o["value"] | true) ? 1.f : 0.f; break;
    case ConditionType::OutputState:
      c.index = o["output"] | 0;
      c.value = (o["value"] | true) ? 1.f : 0.f;
      break;
    case ConditionType::RoomGroup: c.text = o["state"] | "any_heating"; break;
    default: break;
  }
  return true;
}

void ruleToJson(const Rule& r, JsonObject o, bool runtime) {
  o["id"] = r.id;
  o["name"] = r.name;
  o["enabled"] = r.enabled;
  o["testMode"] = r.testMode;
  o["priority"] = r.priority;
  o["cooldownSec"] = r.cooldownSec;
  o["maxPerHour"] = r.maxPerHour;
  JsonArray ts = o["triggers"].to<JsonArray>();
  for (auto& t : r.triggers) triggerToJson(t, ts.add<JsonObject>());
  JsonArray cs = o["conditions"].to<JsonArray>();
  for (auto& c : r.conditions) conditionToJson(c, cs.add<JsonObject>());
  actionsToJson(r.actions, o["actions"].to<JsonArray>());
  o["execCount"] = r.execCount;
  o["lastExecEpoch"] = r.lastExecEpoch;
  if (runtime) {
    o["blockedCount"] = r.blockedCount;
    o["lastResult"] = r.lastResult;
    o["valid"] = r.valid;
    o["validationError"] = r.validationError;
  }
}

bool ruleFromJson(Rule& r, JsonObjectConst o, std::string& err) {
  r = Rule{};
  r.id = o["id"] | 0;
  r.name = o["name"] | "";
  r.enabled = o["enabled"] | false;
  r.testMode = o["testMode"] | false;
  r.priority = o["priority"] | 50;
  r.cooldownSec = o["cooldownSec"] | 5u;
  r.maxPerHour = o["maxPerHour"] | 60;
  for (JsonObjectConst t : o["triggers"].as<JsonArrayConst>()) {
    Trigger x;
    if (!triggerFromJson(x, t, err)) return false;
    r.triggers.push_back(x);
  }
  for (JsonObjectConst c : o["conditions"].as<JsonArrayConst>()) {
    Condition x;
    if (!conditionFromJson(x, c, err)) return false;
    r.conditions.push_back(x);
  }
  if (!actionsFromJson(r.actions, o["actions"].as<JsonArrayConst>(), err)) return false;
  r.execCount = o["execCount"] | 0u;
  r.lastExecEpoch = o["lastExecEpoch"] | static_cast<int64_t>(0);
  return true;
}

}  // namespace hc
