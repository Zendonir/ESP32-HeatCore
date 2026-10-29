#pragma once
// Actions are the only way any signal source (wired input, RF button,
// automation rule, MQTT, web) changes system state. All of them go through
// the same executor in HeatCoreSystem, which applies the safety limits.
#include <ArduinoJson.h>

#include <cstdint>
#include <string>
#include <vector>

#include "hc/core/Event.h"

namespace hc {

enum class ActionType : uint8_t {
  SetSetpoint = 0,     // room, value
  AdjustSetpoint,      // room, value (= delta)
  SetRoomMode,         // room, text (= mode)
  SetGlobalMode,       // text (= mode), durationSec (vacation/away end, 0 = open)
  ActivateRoomGroup,   // target (= group), text (= room mode)
  SetOutput,           // index (= output 1..16), value (0/1), durationSec
  RfSend,              // target (= "<txDeviceId>/<commandId>")
  MqttPublish,         // target (= topic suffix), text (= payload)
  Notify,              // text (= message), value (= level 0..2)
  DisplayPage,         // index (= page 1..5)
  DisplayNext,
  DisplayPrev,
  DisplayWake,
  DisplayRoomNext,     // next room on the room page
  DisplayedRoomAdjust, // value (= delta) on the room currently shown
  RaiseAlarm,          // target (= code), text (= message), value (= level)
  ClearAlarm,          // target (= code)
  WindowState,         // room, value (1 = open, 0 = closed)
  Presence,            // room ("" = home), value (1/0)
  HeatDemand,          // room, value (1/0), durationSec (auto-expire)
  Count
};

const char* actionTypeName(ActionType t);
bool actionTypeFromName(const char* s, ActionType& out);

struct Action {
  ActionType type = ActionType::Notify;
  std::string room;
  std::string target;
  std::string text;
  float value = 0.f;
  int32_t index = -1;
  uint32_t durationSec = 0;
};

// Who triggered an action. depth/ruleId flow into the events the action
// produces so that loops can be detected.
struct ActionOrigin {
  EventSource source = EventSource::System;
  uint8_t depth = 0;
  uint16_t ruleId = 0;
  bool dryRun = false;
};

void actionToJson(const Action& a, JsonObject o);
bool actionFromJson(Action& a, JsonObjectConst o, std::string& err);
void actionsToJson(const std::vector<Action>& v, JsonArray arr);
bool actionsFromJson(std::vector<Action>& v, JsonArrayConst arr, std::string& err);

// Short key identifying the state an action modifies, used for priority
// conflict resolution ("setpoint:wohnen", "output:3", ...). Empty = no conflict.
std::string actionTargetKey(const Action& a);

// Gesture-bound action list used by wired inputs and RF buttons.
struct InputBinding {
  Gesture gesture = Gesture::Short;
  std::vector<Action> actions;
};
void bindingsToJson(const std::vector<InputBinding>& v, JsonArray arr);
bool bindingsFromJson(std::vector<InputBinding>& v, JsonArrayConst arr, std::string& err);

}  // namespace hc
