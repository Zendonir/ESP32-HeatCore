#include "hc/core/Alarms.h"

#include <algorithm>

#include "hc/core/Log.h"

namespace hc {

bool AlarmManager::raise(const std::string& code, const std::string& message, AlarmLevel level, EventSource src,
                         uint8_t depth, uint16_t ruleId) {
  for (auto& a : alarms_) {
    if (a.code == code) {
      ++a.count;
      a.message = message;
      if (level > a.level) a.level = level;
      return false;
    }
  }
  if (alarms_.size() >= 32) alarms_.erase(alarms_.begin());
  Alarm a;
  a.code = code;
  a.message = message;
  a.level = level;
  a.source = src;
  a.sinceMs = clock_.nowMs();
  a.sinceEpoch = clock_.epoch();
  alarms_.push_back(a);
  Event e(EventType::Alarm, src);
  e.ts = a.sinceMs;
  e.depth = depth;
  e.ruleId = ruleId;
  e.setRef(code.c_str());
  e.setText(message.c_str());
  e.value = static_cast<float>(level);
  bus_.post(e);
  HC_LOGW("alarm", "%s: %s", code.c_str(), message.c_str());
  return true;
}

bool AlarmManager::clear(const std::string& code, uint8_t depth, uint16_t ruleId) {
  auto it = std::find_if(alarms_.begin(), alarms_.end(), [&](const Alarm& a) { return a.code == code; });
  if (it == alarms_.end()) return false;
  alarms_.erase(it);
  Event e(EventType::AlarmCleared, EventSource::System);
  e.ts = clock_.nowMs();
  e.depth = depth;
  e.ruleId = ruleId;
  e.setRef(code.c_str());
  bus_.post(e);
  return true;
}

bool AlarmManager::active(const std::string& code) const {
  for (auto& a : alarms_)
    if (a.code == code) return true;
  return false;
}

AlarmLevel AlarmManager::highest() const {
  AlarmLevel h = AlarmLevel::Info;
  for (auto& a : alarms_)
    if (a.level > h) h = a.level;
  return h;
}

void AlarmManager::toJson(JsonArray arr) const {
  for (auto& a : alarms_) {
    JsonObject o = arr.add<JsonObject>();
    o["code"] = a.code;
    o["message"] = a.message;
    o["level"] = static_cast<int>(a.level);
    o["source"] = eventSourceName(a.source);
    o["sinceEpoch"] = a.sinceEpoch;
    o["ageSec"] = elapsed(clock_.nowMs(), a.sinceMs) / 1000u;
    o["count"] = a.count;
  }
}

}  // namespace hc
