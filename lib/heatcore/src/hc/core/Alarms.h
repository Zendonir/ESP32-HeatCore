#pragma once
// Active alarm list. System alarms (safety, sensors, hardware) can only be
// cleared by the system itself; automation rules may only raise/clear alarms
// in the "user." namespace.
#include <ArduinoJson.h>

#include <string>
#include <vector>

#include "hc/core/Clock.h"
#include "hc/core/Event.h"

namespace hc {

enum class AlarmLevel : uint8_t { Info = 0, Warning = 1, Critical = 2 };

struct Alarm {
  std::string code;
  std::string message;
  AlarmLevel level = AlarmLevel::Warning;
  EventSource source = EventSource::System;
  Millis sinceMs = 0;
  int64_t sinceEpoch = 0;
  uint32_t count = 1;
};

class AlarmManager {
 public:
  AlarmManager(EventBus& bus, const IClock& clock) : bus_(bus), clock_(clock) {}
  // Returns true if the alarm is new.
  bool raise(const std::string& code, const std::string& message, AlarmLevel level, EventSource src,
             uint8_t depth = 0, uint16_t ruleId = 0);
  bool clear(const std::string& code, uint8_t depth = 0, uint16_t ruleId = 0);
  bool active(const std::string& code) const;
  const std::vector<Alarm>& list() const { return alarms_; }
  AlarmLevel highest() const;
  size_t count() const { return alarms_.size(); }
  void toJson(JsonArray arr) const;
  static bool isUserCode(const std::string& code) { return code.rfind("user.", 0) == 0; }

 private:
  EventBus& bus_;
  const IClock& clock_;
  std::vector<Alarm> alarms_;
};

}  // namespace hc
