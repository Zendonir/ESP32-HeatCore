#pragma once
// Rule model of the automation engine: triggers, conditions, actions.
#include <ArduinoJson.h>

#include <cstdint>
#include <string>
#include <vector>

#include "hc/automation/Action.h"
#include "hc/core/Clock.h"
#include "hc/core/Event.h"

namespace hc {

enum class TriggerType : uint8_t {
  Input = 0,          // ref = "di:3" | "rf:5/1" | "rf:5" | "*" ; gesture
  TempThreshold,      // ref = room id or "sensor:<id>"; op above/below; value; hysteresis
  TempChange,         // ref; value = delta K (sign = direction); windowSec
  RoomState,          // ref = room id or "*"; state = setpoint|mode|window_open|window_closed|heating_on|heating_off|presence
  ValveState,         // index = circuit (0 = any); state = open|closed|fault
  Time,               // time = minute of day; days = bit mask Mon=bit0
  Mqtt,               // ref = topic suffix; text = payload match ("" = any)
  SensorFault,        // ref = sensor id or "*"
  ModelEvent,         // ref = room id or "*"; state = ready|degraded|fault_suspected
  Manual,             // run from web UI / Home Assistant
  GlobalMode,         // state = mode name or "*"
  Alarm,              // ref = alarm code prefix or "*"
  Count
};

enum class ConditionType : uint8_t {
  Temperature = 0,    // ref = room / "sensor:<id>"; op; value
  Setpoint,           // ref = room; op; value
  TimeRange,          // from, to (minute of day, may wrap midnight)
  Weekday,            // days mask
  GlobalMode,         // text = mode
  RoomMode,           // ref = room; text = mode
  Presence,           // ref = room or "" (home); value 1/0
  Window,             // ref = room; value 1 (open) / 0 (closed)
  SensorAvailable,    // ref = sensor id; value 1/0
  InputState,         // ref = "di:3" | "rf:5"; value 1 (on/open) / 0
  OutputState,        // index = output; value 1/0
  RoomGroup,          // ref = group; text = any_heating|none_heating|all_window_closed
  Count
};

enum class CompareOp : uint8_t { Lt = 0, Le, Eq, Ge, Gt };

const char* triggerTypeName(TriggerType t);
bool triggerTypeFromName(const char* s, TriggerType& out);
const char* conditionTypeName(ConditionType t);
bool conditionTypeFromName(const char* s, ConditionType& out);
const char* compareOpName(CompareOp o);
bool compareOpFromName(const char* s, CompareOp& out);
bool compare(float a, CompareOp op, float b);

struct Trigger {
  TriggerType type = TriggerType::Manual;
  std::string ref;
  Gesture gesture = Gesture::None;  // Input: None = any gesture
  bool above = true;                // TempThreshold
  float value = 0.f;
  float hysteresis = 0.3f;
  uint32_t windowSec = 600;         // TempChange
  std::string state;
  int32_t index = 0;
  uint16_t time = 0;
  uint8_t days = 0x7F;
  std::string text;
  // runtime
  bool armed = true;
  bool haveRef = false;
  float refValue = 0.f;
  Millis refMs = 0;
};

struct Condition {
  ConditionType type = ConditionType::Weekday;
  std::string ref;
  CompareOp op = CompareOp::Ge;
  float value = 0.f;
  uint16_t from = 0, to = 0;
  uint8_t days = 0x7F;
  std::string text;
  int32_t index = 0;
  bool negate = false;
};

struct Rule {
  uint16_t id = 0;
  std::string name;
  bool enabled = false;
  bool testMode = false;         // triggered normally, actions only simulated + logged
  int priority = 50;             // 0..100, higher executes first and wins conflicts
  std::vector<Trigger> triggers; // any trigger fires the rule
  std::vector<Condition> conditions;  // all must hold
  std::vector<Action> actions;
  uint32_t cooldownSec = 5;
  uint16_t maxPerHour = 60;
  // runtime / statistics
  uint32_t execCount = 0;
  uint32_t blockedCount = 0;
  int64_t lastExecEpoch = 0;
  Millis lastExecMs = 0;
  bool executedOnce = false;
  std::string lastResult;
  uint16_t execThisHour = 0;
  Millis hourStart = 0;
  bool valid = false;
  std::string validationError;
};

void triggerToJson(const Trigger& t, JsonObject o);
bool triggerFromJson(Trigger& t, JsonObjectConst o, std::string& err);
void conditionToJson(const Condition& c, JsonObject o);
bool conditionFromJson(Condition& c, JsonObjectConst o, std::string& err);
void ruleToJson(const Rule& r, JsonObject o, bool runtime);
bool ruleFromJson(Rule& r, JsonObjectConst o, std::string& err);

}  // namespace hc
