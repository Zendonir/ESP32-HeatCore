#pragma once
// Rooms, heating circuits, operating modes and the conventional hysteresis
// controller. The learning model may only shift the switching thresholds by a
// bounded amount; without a trusted model this controller runs unchanged.
#include <ArduinoJson.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "hc/core/Clock.h"
#include "hc/core/Event.h"
#include "hc/io/IoManager.h"

namespace hc {

enum class RoomMode : uint8_t { Auto = 0, Comfort, Eco, Frost, Off };
enum class GlobalMode : uint8_t { Normal = 0, Eco, Away, Vacation };
const char* roomModeName(RoomMode m);
bool roomModeFromName(const char* s, RoomMode& out);
const char* globalModeName(GlobalMode m);
bool globalModeFromName(const char* s, GlobalMode& out);

struct Period {
  uint16_t start = 0;  // minute of day
  uint16_t end = 0;
};

struct Room {
  // configuration
  std::string id;
  std::string name;
  float comfort = 21.f;
  float eco = 18.f;
  float frost = 7.f;
  RoomMode mode = RoomMode::Auto;
  float hysteresis = 0.4f;
  std::vector<std::string> groups;
  std::array<std::vector<Period>, 7> schedule;  // comfort periods, Monday first
  bool modelAssist = true;
  uint32_t manualOverrideMin = 180;  // manual setpoint lifetime (also ends at schedule change)
  bool showOnDisplay = true;

  // runtime
  std::vector<std::string> windowSources;  // refs of currently open contacts
  bool windowOpen() const { return !windowSources.empty(); }
  int8_t presence = -1;  // -1 unknown, 0 absent, 1 present
  bool manualActive = false;
  float manualSetpoint = 0.f;
  Millis manualUntil = 0;
  int manualSlot = -1;
  bool externalDemand = false;
  Millis externalDemandUntil = 0;
  std::vector<std::string> demandSources;

  bool hasTemp = false;
  float temperature = 0.f;
  std::string tempSensor;
  bool tempFallback = false;
  float setpoint = 0.f;  // effective
  std::string reason;
  bool demand = false;
  Millis demandChangedMs = 0;
  bool sensorFail = false;
  bool overTemp = false;
  float assistOnShift = 0.f, assistOffShift = 0.f;  // applied model advice (K)
};

struct Circuit {
  int index = 0;  // 1..16, uses output with the same number
  std::string name;
  std::string room;
  bool enabled = true;
  bool valveNormallyOpen = false;
  uint16_t minOnSec = 120;
  uint16_t minOffSec = 120;
  // runtime
  bool request = false;
  bool open = false;
  bool fault = false;
  std::string faultText;
  Millis lastSwitchMs = 0;
  bool switched = false;
  Millis openSinceMs = 0;
};

struct ModelAdvice {
  bool valid = false;
  float startEarlyK = 0.f;  // raise the switch-on threshold by this amount
  float stopEarlyK = 0.f;   // lower the switch-off threshold by this amount
};

enum class SensorFailMode : uint8_t { Off = 0, Duty };

class HeatingManager {
 public:
  HeatingManager(EventBus& bus, const IClock& clock);

  std::vector<Room> rooms;
  std::array<Circuit, kNumOutputs> circuits;
  GlobalMode globalMode = GlobalMode::Normal;
  int64_t globalModeUntilEpoch = 0;  // 0 = open ended
  Millis globalModeUntilMs = 0;
  bool globalModeTimed = false;

  Room* room(const std::string& id);
  const Room* room(const std::string& id) const;
  std::vector<Room*> roomsInGroup(const std::string& group);
  std::vector<std::string> groups() const;

  // Commands (all values are clamped by the caller's safety limits)
  bool setManualSetpoint(Room& r, float sp, float minSp, float maxSp, Millis now, uint8_t depth, uint16_t ruleId,
                         EventSource src);
  bool adjustSetpoint(Room& r, float delta, float minSp, float maxSp, Millis now, uint8_t depth, uint16_t ruleId,
                      EventSource src);
  bool setRoomMode(Room& r, RoomMode m, uint8_t depth, uint16_t ruleId, EventSource src);
  void setGlobalMode(GlobalMode m, uint32_t durationSec, Millis now, uint8_t depth, uint16_t ruleId, EventSource src);
  void setWindow(Room& r, const std::string& source, bool open, Millis now, uint8_t depth, uint16_t ruleId,
                 EventSource src);
  void setPresence(Room* r, bool present, Millis now, uint8_t depth, uint16_t ruleId, EventSource src);
  void setExternalDemand(Room& r, const std::string& source, bool on, uint32_t durationSec, Millis now);
  bool homePresent() const;

  // Control cycle
  float computeSetpoint(Room& r, const LocalTime& lt, float minSp, float maxSp, Millis now);
  void updateDemand(Room& r, const ModelAdvice& adv, SensorFailMode failMode, uint8_t dutyPct, uint16_t cycleMin,
                    Millis now);
  // Applies room demand to circuits honouring minimum on/off times.
  void updateCircuits(Millis now);
  // Logical output level for a circuit (NO valves are inverted).
  bool circuitOutputLevel(const Circuit& c) const { return c.valveNormallyOpen ? !c.open : c.open; }
  // Output level that closes the valve (used by safety and watchdog).
  bool circuitSafeLevel(const Circuit& c) const { return c.valveNormallyOpen; }

  static int scheduleSlot(const Room& r, const LocalTime& lt, bool* comfort);

  void toJson(JsonObject o, bool runtime) const;
  std::string fromJson(JsonObjectConst o);
  static std::string validateRoomId(const std::string& id);
  uint32_t revision() const { return revision_; }
  void touch() { ++revision_; }

 private:
  void emitRoom(EventType t, const Room& r, float value, Millis now, uint8_t depth, uint16_t ruleId,
                EventSource src);
  EventBus& bus_;
  const IClock& clock_;
  uint32_t revision_ = 0;
};

std::string formatHHMM(uint16_t minuteOfDay);
bool parseHHMM(const char* s, uint16_t& out);

}  // namespace hc
