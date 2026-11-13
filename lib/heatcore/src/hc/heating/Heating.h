#pragma once
// Rooms, heating circuits, operating modes and the room controllers:
//  * hysteresis (two-point): valve open below setpoint - h/2, closed above
//    setpoint + h/2. The learning model may only shift the switching
//    thresholds by a bounded amount; without a trusted model it runs unchanged.
//  * PWM (time proportional, PI): thermal actuators only know open/closed, so
//    a PI controller computes a duty cycle (0..100 %) that is applied as one
//    on-pulse per cycle (e.g. 35 % of 20 min = 7 min open). Pulses shorter than
//    the valve minimum on/off times (= actuator travel time) are rounded to
//    0 % / minimum pulse / 100 %.
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
enum class RoomControl : uint8_t { Hysteresis = 0, Pwm };
const char* roomModeName(RoomMode m);
bool roomModeFromName(const char* s, RoomMode& out);
const char* globalModeName(GlobalMode m);
bool globalModeFromName(const char* s, GlobalMode& out);
const char* roomControlName(RoomControl c);
bool roomControlFromName(const char* s, RoomControl& out);

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
  RoomControl control = RoomControl::Hysteresis;
  uint16_t pwmCycleMin = 20;  // PWM cycle length (5..60 min)
  float pwmKp = 40.f;         // proportional gain in % duty per K below setpoint (5..200)
  uint16_t pwmTnMin = 90;     // integral (reset) time in min, 0 = P only (0..600)

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
  // PWM controller state
  float duty = 0.f;      // current PI output in %
  float pwmI = 0.f;      // integral part in %
  bool pwmRunning = false;
  Millis pwmLastMs = 0;  // last PI update
  Millis pwmCycleStartMs = 0;
  uint32_t pwmOnMs = 0;  // on-time latched for the running cycle
  float pwmCycleSetpoint = 0.f;
};

// Permanent manual control of a heating circuit (valve), kept across restarts.
// Percent: open for that share of a cycle (time-proportional, like PWM rooms).
enum class CircuitManual : uint8_t { Auto = 0, Open, Closed, Percent };
const char* circuitManualName(CircuitManual m);
bool circuitManualFromName(const char* s, CircuitManual& out);

struct Circuit {
  int index = 0;  // 1..16, uses output with the same number
  std::string name;
  std::string room;
  bool enabled = true;
  bool valveNormallyOpen = false;
  uint16_t minOnSec = 120;
  uint16_t minOffSec = 120;
  // Share of the room's heat this circuit gets (rooms with several
  // circuits): PWM rooms - this share of the room's on-time; two-point rooms -
  // open for this share of each cycle while the room heats. 100 = like the room.
  uint8_t share = 100;
  CircuitManual manual = CircuitManual::Auto;
  uint8_t manualPct = 50;         // Percent: open share 0..100 %
  uint16_t manualCycleMin = 20;   // Percent: cycle length 5..120 min
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
  // false while the heat source delivers no heat: automatic circuits stay
  // closed (manual control is not affected)
  bool supplyOk = true;
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
  // Changes the stored comfort or eco temperature ("comfort" | "eco") within
  // minSp..maxSp and frost <= eco <= comfort. Returns an error text or "".
  std::string setPresetTemp(Room& r, const std::string& which, float v, float minSp, float maxSp, Millis now,
                            EventSource src);
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
  // permanent manual control of a circuit: wanted valve state now, validation
  static bool manualWants(const Circuit& c, Millis now);
  // automatic mode: does circuit c open now, given its room heats (share < 100)?
  static bool shareWants(const Circuit& c, const Room& r, Millis now);
  static std::string validateManual(const Circuit& c);
  bool circuitOutputLevel(const Circuit& c) const { return c.valveNormallyOpen ? !c.open : c.open; }
  // Output level that closes the valve (used by safety and watchdog).
  bool circuitSafeLevel(const Circuit& c) const { return c.valveNormallyOpen; }

  static int scheduleSlot(const Room& r, const LocalTime& lt, bool* comfort);
  // Shortest useful valve pulse for a room: the longest minimum on/off time
  // of its circuits (thermal actuators need minutes to travel).
  uint32_t minPulseMs(const Room& r) const;

  void toJson(JsonObject o, bool runtime) const;
  std::string fromJson(JsonObjectConst o);
  static std::string validateRoomId(const std::string& id);
  uint32_t revision() const { return revision_; }
  void touch() { ++revision_; }

 private:
  bool pwmDemand(Room& r, Millis now);
  void emitRoom(EventType t, const Room& r, float value, Millis now, uint8_t depth, uint16_t ruleId,
                EventSource src);
  EventBus& bus_;
  const IClock& clock_;
  uint32_t revision_ = 0;
};

std::string formatHHMM(uint16_t minuteOfDay);
bool parseHHMM(const char* s, uint16_t& out);

}  // namespace hc
