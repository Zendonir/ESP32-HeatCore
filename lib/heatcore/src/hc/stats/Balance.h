#pragma once
// Hydraulic balance assistant.
//
// A room thermostat that switches a valve on and off cannot measure flow, but
// the recorded valve open time tells how hard a circuit has to work to hold
// its setpoint. In a balanced system every room needs about the same share of
// valve time; a room whose valve is open nearly all the time and still stays
// cold gets too little flow, a room that holds its setpoint with the valve
// open only briefly gets more than it needs (and takes flow from the others).
//
// The analysis uses the 15-minute history of the last days and only counts
// steady comfort periods: setpoint at least 15 degC, no window, no manual
// operation, no fault or sensor fallback, heat supply available, outdoor
// temperature (if known) at most 15 degC and not within two hours after a
// setpoint step (warm-up). After a manual adjustment at the manifold (noted in
// the change log) the room waits a settling day and is then assessed on the
// new data only.
#include <ArduinoJson.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "hc/stats/History.h"

namespace hc {

struct BalanceNote {
  uint32_t id = 0;    // unique within the log (delete key)
  uint32_t tMin = 0;  // epoch minutes (UTC)
  std::string room;   // room id, empty = all rooms
  std::string text;   // e.g. "Kreis 3: 1,5 -> 2,0 l/min"
};

struct BalanceConfig {
  uint8_t days = 7;                // analysis window 3..14 days
  std::vector<BalanceNote> notes;  // oldest first, at most kMaxNotes
  static constexpr size_t kMaxNotes = 40;
  static constexpr size_t kMaxText = 120;

  void toJson(JsonObject o) const;
  std::string fromJson(JsonObjectConst o);  // keeps fields that are absent
  void addNote(uint32_t tMin, const std::string& room, const std::string& text);
};

enum class BalanceVerdict : uint8_t { NoData = 0, Ok, IncreaseSlight, Increase, Reduce };
const char* balanceVerdictName(BalanceVerdict v);   // nodata | ok | increase_slight | increase | reduce
const char* balanceVerdictLabel(BalanceVerdict v);  // German short text

enum class BalanceSystem : uint8_t { None = 0, Ok, FlowLow, FlowHigh };
const char* balanceSystemName(BalanceSystem s);  // none | ok | flow_low | flow_high

struct RoomBalance {
  std::string room;
  float hours = 0.f;      // suitable hours
  float duty = NAN;       // mean valve open time [%]
  float dev = NAN;        // mean temperature - setpoint [K]
  float belowPct = 0.f;   // share of suitable time more than 0.5 K below the setpoint
  float abovePct = 0.f;   // share more than 0.5 K above
  float outdoor = NAN;    // mean outdoor temperature in the counted periods
  uint32_t sinceMin = 0;  // counted from (window start or after the last note + settling)
  bool afterNote = false; // sinceMin comes from a change-log entry
  bool externalHeat = false;  // warmer than wanted with the valve rarely open
  BalanceVerdict verdict = BalanceVerdict::NoData;
};

struct BalanceResult {
  std::vector<RoomBalance> rooms;
  BalanceSystem system = BalanceSystem::None;
  float medianDuty = NAN;
  int assessed = 0;  // rooms with enough data
};

class BalanceAnalyzer {
 public:
  static constexpr float kMinHours = 12.f;
  static constexpr uint32_t kSettleMin = 24u * 60u;
  static constexpr uint32_t kWarmupSkipMin = 120u;
  static constexpr float kMaxOutdoor = 15.f;

  // pts: 15-minute buckets of one room, oldest first; buckets before sinceMin are ignored
  static RoomBalance analyzeRoom(const std::string& room, const std::vector<HistPoint>& pts, uint32_t sinceMin);
  // sets the verdict of every room and the system hint
  static void classify(BalanceResult& r);
  // German advice for a room (multi = room has several circuits)
  static std::string advice(const RoomBalance& rb, BalanceSystem sys, bool multi);
  static std::string systemAdvice(BalanceSystem sys);
};

}  // namespace hc
