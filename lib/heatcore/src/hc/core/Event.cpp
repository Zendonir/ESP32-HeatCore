#include "hc/core/Event.h"

namespace hc {

namespace {
const char* const kTypeNames[] = {
    "none",          "input",          "rf_frame",          "rf_unknown",       "rf_sent",
    "rf_send_failed", "sensor_value",  "sensor_fault",      "sensor_recovered", "room_setpoint",
    "room_mode",     "room_heating",   "room_window",       "room_presence",    "valve",
    "circuit_fault", "output",         "global_mode",       "time_minute",      "mqtt",
    "model",         "manual",         "alarm",             "alarm_cleared",    "notification",
    "display_page",  "safety_trip",    "config_changed"};
static_assert(sizeof(kTypeNames) / sizeof(kTypeNames[0]) == static_cast<size_t>(EventType::Count),
              "event type name table out of sync");

const char* const kSourceNames[] = {"system", "di",       "rf",     "sensor", "web",    "mqtt",
                                    "automation", "schedule", "safety", "model",  "display", "ai"};
static_assert(sizeof(kSourceNames) / sizeof(kSourceNames[0]) == static_cast<size_t>(EventSource::Count),
              "event source name table out of sync");

const char* const kGestureNames[] = {"none",   "press",  "release",     "short",    "long",
                                     "open",   "closed", "unavailable", "available"};
static_assert(sizeof(kGestureNames) / sizeof(kGestureNames[0]) == static_cast<size_t>(Gesture::Count),
              "gesture name table out of sync");
}  // namespace

const char* eventTypeName(EventType t) {
  auto i = static_cast<size_t>(t);
  return i < static_cast<size_t>(EventType::Count) ? kTypeNames[i] : "?";
}
const char* eventSourceName(EventSource s) {
  auto i = static_cast<size_t>(s);
  return i < static_cast<size_t>(EventSource::Count) ? kSourceNames[i] : "?";
}
const char* gestureName(Gesture g) {
  auto i = static_cast<size_t>(g);
  return i < static_cast<size_t>(Gesture::Count) ? kGestureNames[i] : "?";
}
EventType eventTypeFromName(const char* s) {
  if (!s) return EventType::None;
  for (size_t i = 0; i < static_cast<size_t>(EventType::Count); ++i)
    if (std::strcmp(s, kTypeNames[i]) == 0) return static_cast<EventType>(i);
  return EventType::None;
}
Gesture gestureFromName(const char* s) {
  if (!s) return Gesture::None;
  for (size_t i = 0; i < static_cast<size_t>(Gesture::Count); ++i)
    if (std::strcmp(s, kGestureNames[i]) == 0) return static_cast<Gesture>(i);
  return Gesture::None;
}

EventBus::EventBus(size_t queueCapacity, size_t historyCapacity)
    : queueCap_(queueCapacity), historyCap_(historyCapacity) {}

bool EventBus::post(Event e) {
  std::lock_guard<std::mutex> lk(mtx_);
  if (queue_.size() >= queueCap_) {
    ++dropped_;
    return false;
  }
  e.seq = ++seq_;
  queue_.push_back(e);
  ++posted_;
  return true;
}

bool EventBus::pop(Event& out) {
  std::lock_guard<std::mutex> lk(mtx_);
  if (queue_.empty()) return false;
  out = queue_.front();
  queue_.pop_front();
  return true;
}

size_t EventBus::pending() const {
  std::lock_guard<std::mutex> lk(mtx_);
  return queue_.size();
}

void EventBus::record(const Event& e) {
  std::lock_guard<std::mutex> lk(mtx_);
  if (history_.size() >= historyCap_) history_.pop_front();
  history_.push_back(e);
}

std::vector<Event> EventBus::history(uint32_t sinceSeq, size_t maxCount) const {
  std::lock_guard<std::mutex> lk(mtx_);
  std::vector<Event> out;
  for (auto it = history_.rbegin(); it != history_.rend() && out.size() < maxCount; ++it) {
    if (it->seq <= sinceSeq) break;
    out.push_back(*it);
  }
  return std::vector<Event>(out.rbegin(), out.rend());
}

uint32_t EventBus::lastSeq() const {
  std::lock_guard<std::mutex> lk(mtx_);
  return seq_;
}

}  // namespace hc
