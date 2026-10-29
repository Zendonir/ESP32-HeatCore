#pragma once
// Recorded operating history for analysis (web UI, REST, MCP / AI tools).
//
// Every room gets 15-minute buckets built from the one-minute observations
// of the control loop: mean temperature, mean effective setpoint, valve duty,
// outdoor temperature, window minutes, valve switch-ons and flags. 14 days
// are kept in a ring buffer per room (16 bytes per bucket); the series
// "_system" holds flow and outdoor temperature. The buffers survive restarts
// through a compact binary snapshot (serialize / deserialize).
//
// RoomKpis condenses a period into numbers that describe how well a room is
// controlled: comfort deviation, time below/above the setpoint, overshoot,
// warm-up time after a setpoint step, heating hours and switching cycles.
#include <ArduinoJson.h>

#include <climits>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "hc/core/Clock.h"

namespace hc {

constexpr int16_t kHistNoValue = INT16_MIN;
constexpr uint16_t kHistBucketMin = 15;
constexpr uint16_t kHistDays = 14;
constexpr uint16_t kHistCapacity = kHistDays * 24 * 60 / kHistBucketMin;  // 1344
constexpr const char* kHistSystemSeries = "_system";

enum HistFlag : uint8_t {
  kHistManualSetpoint = 1,
  kHistManualOutput = 2,
  kHistFault = 4,
  kHistSensorFallback = 8,
  kHistComfortSlot = 16,
  kHistModelAssist = 32,  // model advice shifted the switching thresholds
};

struct HistPoint {
  uint32_t tMin = 0;  // bucket start, epoch minutes (UTC); 0 = time unknown
  int16_t temp = kHistNoValue;      // 1/100 degC (system series: flow temperature)
  int16_t setpoint = kHistNoValue;  // 1/100 degC
  int16_t outdoor = kHistNoValue;   // 1/100 degC
  uint8_t duty = 0;                 // valve open time 0..100 %
  uint8_t windowMin = 0;
  uint8_t minutes = 0;              // minutes contained (<= 15)
  uint8_t flags = 0;
  uint8_t switches = 0;             // valve switch-ons
  uint8_t reserved = 0;
};
static_assert(sizeof(HistPoint) == 16, "HistPoint layout");

struct HistSample {  // one minute of one room
  bool tempValid = false;
  float temp = 0.f;
  bool setpointValid = false;
  float setpoint = 0.f;
  bool outdoorValid = false;
  float outdoor = 0.f;
  float duty = 0.f;  // 0..1
  bool windowOpen = false;
  uint8_t flags = 0;
  uint8_t switches = 0;
};

struct RoomKpis {
  float hours = 0.f;              // hours with temperature data
  float meanTemp = NAN, meanSetpoint = NAN, meanOutdoor = NAN;
  float comfortMeanDev = NAN;     // mean (T - setpoint) in comfort periods [K]
  float comfortMeanAbsDev = NAN;  // mean |T - setpoint| in comfort periods [K]
  float belowHours = 0.f;         // T < setpoint - 0.5 K (window/fault excluded)
  float aboveHours = 0.f;         // T > setpoint + 0.5 K while the valve was closed
  float maxOvershootK = NAN;      // highest T - setpoint after heating stopped
  float heatingHours = 0.f;       // valve open time
  uint32_t switches = 0;
  float windowHours = 0.f;
  float faultHours = 0.f;
  float manualHours = 0.f;
  uint32_t warmups = 0;           // setpoint steps >= 1 K upwards
  float warmupMeanMin = NAN;      // time until T >= new setpoint - 0.3 K
  float warmupMaxMin = NAN;
  uint32_t warmupsNotReached = 0; // not reached within 6 h
};

class History {
 public:
  explicit History(uint16_t capacity = kHistCapacity) : cap_(capacity) {}

  // One-minute sample; epochMin = 0 when the time is unknown.
  void record(const std::string& series, const HistSample& s, uint32_t epochMin);
  void keepOnly(const std::vector<std::string>& series);  // drops removed rooms (system series kept)
  void clear();

  std::vector<std::string> seriesNames() const;
  size_t points(const std::string& series) const;
  // Completed buckets of the last `hours` (15-minute resolution, oldest first).
  std::vector<HistPoint> query(const std::string& series, uint32_t hours) const;
  // Merges consecutive buckets to `resolutionMin` (multiple of 15, max 240).
  static std::vector<HistPoint> downsample(const std::vector<HistPoint>& pts, uint16_t resolutionMin);
  // KPIs over 15-minute buckets in chronological order.
  static RoomKpis computeKpis(const std::vector<HistPoint>& pts);

  // JSON helpers used by REST and MCP. utcOffsetSec shifts timestamps to
  // local time for the "local" strings and day boundaries.
  static void pointsToJson(const std::vector<HistPoint>& pts, JsonObject o, int32_t utcOffsetSec);
  static void kpisToJson(const RoomKpis& k, JsonObject o);
  // Per-day KPIs (local days) for the last `days` plus the total.
  void dailyKpisToJson(const std::string& series, uint32_t nowEpochMin, int32_t utcOffsetSec, uint16_t days,
                       JsonObject o) const;

  std::string serialize() const;
  bool deserialize(const std::string& blob);
  uint32_t revision() const { return revision_; }  // completed buckets (for persistence)

 private:
  struct Acc {
    uint32_t bucket = 0;  // bucket start (epoch minutes) or running counter without time
    bool open = false;
    float tSum = 0, spSum = 0, outSum = 0, dutySum = 0;
    uint8_t tN = 0, spN = 0, outN = 0, minutes = 0, windowMin = 0, flags = 0;
    uint16_t switches = 0;
  };
  struct Series {
    std::string name;
    std::vector<HistPoint> ring;
    size_t head = 0;  // next write position
    size_t size = 0;
    Acc acc;
  };
  Series* find(const std::string& name);
  const Series* find(const std::string& name) const;
  void flush(Series& s);
  void push(Series& s, const HistPoint& p);
  std::vector<HistPoint> ordered(const Series& s) const;

  uint16_t cap_;
  std::vector<Series> series_;
  uint32_t revision_ = 0;
  uint32_t noTimeCounter_ = 0;
};

// Seconds between local time and UTC as reported by the clock (0 if unknown).
int32_t utcOffsetSec(const IClock& clock);
std::string formatLocalMinute(uint32_t epochMin, int32_t utcOffsetSec);  // "2026-10-29 14:15"

}  // namespace hc
