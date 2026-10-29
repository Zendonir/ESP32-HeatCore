#include "hc/automation/Action.h"

#include <cstring>

namespace hc {

namespace {
const char* const kActionNames[] = {"set_setpoint",   "adjust_setpoint", "set_room_mode",  "set_global_mode",
                                    "activate_room_group", "set_output", "rf_send",        "mqtt_publish",
                                    "notify",         "display_page",    "display_next",   "display_prev",
                                    "display_wake",   "display_room_next", "displayed_room_adjust",
                                    "raise_alarm",    "clear_alarm",     "window_state",   "presence",
                                    "heat_demand"};
static_assert(sizeof(kActionNames) / sizeof(kActionNames[0]) == static_cast<size_t>(ActionType::Count),
              "action name table out of sync");

std::string str(JsonObjectConst o, const char* key) {
  const char* s = o[key] | "";
  return std::string(s);
}
}  // namespace

const char* actionTypeName(ActionType t) {
  auto i = static_cast<size_t>(t);
  return i < static_cast<size_t>(ActionType::Count) ? kActionNames[i] : "?";
}

bool actionTypeFromName(const char* s, ActionType& out) {
  if (!s) return false;
  for (size_t i = 0; i < static_cast<size_t>(ActionType::Count); ++i) {
    if (std::strcmp(s, kActionNames[i]) == 0) {
      out = static_cast<ActionType>(i);
      return true;
    }
  }
  return false;
}

void actionToJson(const Action& a, JsonObject o) {
  o["type"] = actionTypeName(a.type);
  switch (a.type) {
    case ActionType::SetSetpoint: o["room"] = a.room; o["value"] = a.value; break;
    case ActionType::AdjustSetpoint: o["room"] = a.room; o["delta"] = a.value; break;
    case ActionType::SetRoomMode: o["room"] = a.room; o["mode"] = a.text; break;
    case ActionType::SetGlobalMode:
      o["mode"] = a.text;
      if (a.durationSec) o["durationSec"] = a.durationSec;
      break;
    case ActionType::ActivateRoomGroup: o["group"] = a.target; o["mode"] = a.text; break;
    case ActionType::SetOutput:
      o["output"] = a.index;
      o["state"] = a.value != 0.f;
      if (a.durationSec) o["durationSec"] = a.durationSec;
      break;
    case ActionType::RfSend: o["command"] = a.target; break;
    case ActionType::MqttPublish: o["topic"] = a.target; o["payload"] = a.text; break;
    case ActionType::Notify: o["message"] = a.text; o["level"] = static_cast<int>(a.value); break;
    case ActionType::DisplayPage: o["page"] = a.index; break;
    case ActionType::DisplayedRoomAdjust: o["delta"] = a.value; break;
    case ActionType::RaiseAlarm:
      o["code"] = a.target;
      o["message"] = a.text;
      o["level"] = static_cast<int>(a.value);
      break;
    case ActionType::ClearAlarm: o["code"] = a.target; break;
    case ActionType::WindowState: o["room"] = a.room; o["open"] = a.value != 0.f; break;
    case ActionType::Presence: o["room"] = a.room; o["present"] = a.value != 0.f; break;
    case ActionType::HeatDemand:
      o["room"] = a.room;
      o["on"] = a.value != 0.f;
      if (a.durationSec) o["durationSec"] = a.durationSec;
      break;
    default: break;
  }
}

bool actionFromJson(Action& a, JsonObjectConst o, std::string& err) {
  a = Action{};
  if (!actionTypeFromName(o["type"] | "", a.type)) {
    err = std::string("unbekannter Aktionstyp: ") + (o["type"] | "");
    return false;
  }
  a.room = str(o, "room");
  a.durationSec = o["durationSec"] | 0u;
  switch (a.type) {
    case ActionType::SetSetpoint: a.value = o["value"] | 0.f; break;
    case ActionType::AdjustSetpoint:
    case ActionType::DisplayedRoomAdjust: a.value = o["delta"] | 0.f; break;
    case ActionType::SetRoomMode:
    case ActionType::SetGlobalMode: a.text = str(o, "mode"); break;
    case ActionType::ActivateRoomGroup: a.target = str(o, "group"); a.text = str(o, "mode"); break;
    case ActionType::SetOutput:
      a.index = o["output"] | -1;
      a.value = (o["state"] | false) ? 1.f : 0.f;
      break;
    case ActionType::RfSend: a.target = str(o, "command"); break;
    case ActionType::MqttPublish: a.target = str(o, "topic"); a.text = str(o, "payload"); break;
    case ActionType::Notify: a.text = str(o, "message"); a.value = o["level"] | 0; break;
    case ActionType::DisplayPage: a.index = o["page"] | -1; break;
    case ActionType::RaiseAlarm:
      a.target = str(o, "code");
      a.text = str(o, "message");
      a.value = o["level"] | 1;
      break;
    case ActionType::ClearAlarm: a.target = str(o, "code"); break;
    case ActionType::WindowState: a.value = (o["open"] | false) ? 1.f : 0.f; break;
    case ActionType::Presence: a.value = (o["present"] | false) ? 1.f : 0.f; break;
    case ActionType::HeatDemand: a.value = (o["on"] | false) ? 1.f : 0.f; break;
    default: break;
  }
  return true;
}

void actionsToJson(const std::vector<Action>& v, JsonArray arr) {
  for (const auto& a : v) actionToJson(a, arr.add<JsonObject>());
}

bool actionsFromJson(std::vector<Action>& v, JsonArrayConst arr, std::string& err) {
  v.clear();
  for (JsonObjectConst o : arr) {
    Action a;
    if (!actionFromJson(a, o, err)) return false;
    v.push_back(a);
  }
  return true;
}

std::string actionTargetKey(const Action& a) {
  switch (a.type) {
    case ActionType::SetSetpoint:
    case ActionType::AdjustSetpoint: return "setpoint:" + a.room;
    case ActionType::SetRoomMode: return "roommode:" + a.room;
    case ActionType::SetGlobalMode: return "globalmode";
    case ActionType::ActivateRoomGroup: return "group:" + a.target;
    case ActionType::SetOutput: return "output:" + std::to_string(a.index);
    case ActionType::DisplayPage:
    case ActionType::DisplayNext:
    case ActionType::DisplayPrev:
    case ActionType::DisplayRoomNext: return "display";
    case ActionType::DisplayedRoomAdjust: return "setpoint:@display";
    case ActionType::WindowState: return "window:" + a.room;
    case ActionType::Presence: return "presence:" + a.room;
    case ActionType::HeatDemand: return "demand:" + a.room;
    default: return std::string();
  }
}

void bindingsToJson(const std::vector<InputBinding>& v, JsonArray arr) {
  for (const auto& b : v) {
    JsonObject o = arr.add<JsonObject>();
    o["gesture"] = gestureName(b.gesture);
    actionsToJson(b.actions, o["actions"].to<JsonArray>());
  }
}

bool bindingsFromJson(std::vector<InputBinding>& v, JsonArrayConst arr, std::string& err) {
  v.clear();
  for (JsonObjectConst o : arr) {
    InputBinding b;
    b.gesture = gestureFromName(o["gesture"] | "short");
    if (b.gesture == Gesture::None) {
      err = "unbekannte Geste";
      return false;
    }
    if (!actionsFromJson(b.actions, o["actions"].as<JsonArrayConst>(), err)) return false;
    v.push_back(std::move(b));
  }
  return true;
}

}  // namespace hc
