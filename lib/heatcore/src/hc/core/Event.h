#pragma once
// Unified logical events. Every signal source (wired digital inputs, RF433,
// sensors, MQTT, web, schedule, model) produces the same Event structure so
// that input bindings and the automation engine need exactly one code path.
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "hc/core/Clock.h"

namespace hc {

enum class EventType : uint8_t {
  None = 0,
  InputGesture,        // DI or RF button/contact gesture (see Gesture)
  RfFrame,             // decoded RF frame (known device or not), for monitor/stats
  RfUnknownCode,       // decoded but not registered and not ignored
  RfSent,              // transmitter finished a job
  RfSendFailed,
  SensorValue,         // a sensor delivered a new (valid) value
  SensorFault,         // sensor timed out / failed
  SensorRecovered,
  RoomSetpointChanged,
  RoomModeChanged,
  RoomHeatingChanged,  // heat demand on/off
  RoomWindowChanged,
  RoomPresenceChanged,
  ValveChanged,        // circuit valve output changed
  CircuitFault,
  OutputChanged,       // any physical output changed
  GlobalModeChanged,
  TimeMinute,          // emitted once per wall-clock minute
  MqttMessage,
  ModelEvent,
  ManualTrigger,       // "run" from web UI or Home Assistant
  Alarm,
  AlarmCleared,
  Notification,
  DisplayPageChanged,
  SafetyTrip,
  ConfigChanged,
  Count
};

enum class EventSource : uint8_t {
  System = 0,
  DigitalInput,
  Rf,
  Sensor,
  Web,
  Mqtt,
  Automation,
  Schedule,
  Safety,
  Model,
  Display,
  Ai,  // settings changed through the MCP / AI interface
  Count
};

// Gestures shared by wired inputs and RF devices.
enum class Gesture : uint8_t {
  None = 0,
  Press,        // first frame / edge
  Release,      // end of press
  Short,        // released before long-press threshold
  Long,         // held beyond long-press threshold (emitted once while held)
  Open,         // contact opened
  Closed,       // contact closed
  Unavailable,  // supervision timeout (no telegram / sensor lost)
  Available,    // supervision recovered
  Count
};

const char* eventTypeName(EventType t);
const char* eventSourceName(EventSource s);
const char* gestureName(Gesture g);
EventType eventTypeFromName(const char* s);
Gesture gestureFromName(const char* s);

struct Event {
  EventType type = EventType::None;
  EventSource source = EventSource::System;
  Gesture gesture = Gesture::None;
  uint8_t depth = 0;   // causal depth: 0 = external origin, +1 per automation hop
  uint16_t ruleId = 0; // automation rule that produced this event (0 = none)
  Millis ts = 0;
  uint32_t seq = 0;    // assigned by the bus
  int32_t a = -1;      // numeric id 1: input no., RF device id, circuit, output ...
  int32_t b = -1;      // numeric id 2: RF button index ...
  float value = 0.f;
  float value2 = 0.f;
  char ref[24] = {0};  // string id: room id, sensor id, alarm code ...
  char text[72] = {0}; // free text: MQTT topic+payload, messages

  Event() = default;
  Event(EventType t, EventSource s) : type(t), source(s) {}
  Event& setRef(const char* r) {
    std::strncpy(ref, r ? r : "", sizeof(ref) - 1);
    ref[sizeof(ref) - 1] = 0;
    return *this;
  }
  Event& setText(const char* t) {
    std::strncpy(text, t ? t : "", sizeof(text) - 1);
    text[sizeof(text) - 1] = 0;
    return *this;
  }
};

// Thread-safe bounded queue plus a history ring for diagnostics.
class EventBus {
 public:
  explicit EventBus(size_t queueCapacity = 64, size_t historyCapacity = 200);

  // Enqueue for processing. Returns false (and counts a drop) when full.
  bool post(Event e);
  bool pop(Event& out);
  size_t pending() const;

  // History of processed events (written by the dispatcher).
  void record(const Event& e);
  std::vector<Event> history(uint32_t sinceSeq, size_t maxCount) const;
  uint32_t lastSeq() const;
  uint32_t dropped() const { return dropped_; }
  uint32_t posted() const { return posted_; }

 private:
  mutable std::mutex mtx_;
  std::deque<Event> queue_;
  std::deque<Event> history_;
  size_t queueCap_, historyCap_;
  uint32_t seq_ = 0;
  uint32_t dropped_ = 0;
  uint32_t posted_ = 0;
};

}  // namespace hc
