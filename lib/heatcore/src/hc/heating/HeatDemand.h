#pragma once
// Heat demand: tells the heat source (boiler, heating circuit, pump) when
// at least one of HeatCore's rooms needs heat.
//
// Typical use: a Buderus/Junkers RC35 controls the underfloor heating as
// heating circuit 2 via EMS-ESP; HeatCore switches that circuit to "day" while
// a room needs heat and back to "night" (switch-off mode on the RC35) after a
// run-on time. Heating circuit 1 and the boiler stay with the RC35. The
// command is an MQTT message (any topic/payload) and/or a general output
// (pump, boiler contact).
#include <ArduinoJson.h>

#include <cstdint>
#include <string>
#include <vector>

#include "hc/core/Clock.h"

namespace hc {

struct HeatDemandConfig {
  bool enabled = false;
  std::vector<std::string> rooms;  // rooms that count; empty = all rooms
  uint16_t runOnSec = 600;         // keep requesting after the last demand ended
  uint16_t minOnSec = 600;         // shortest on-period (no short cycling)
  uint16_t minOffSec = 300;        // shortest off-period
  uint16_t repeatSec = 300;        // resend the current state (0 = only on change)
  int output = 0;                  // general output 1..16, 0 = none
  // "mqtt": free MQTT message (topic/payloads below)
  // "emsesp": heating circuit of the EMS-ESP connection, switched directly
  //           (day / night with switch-off reduction, given back with auto)
  std::string target = "mqtt";
  std::string topic;               // full MQTT topic, empty = no MQTT
  std::string payloadOn, payloadOff;
  std::string payloadFallback;     // sent when HeatCore gives control back (disabled, restart)
  bool retain = false;
};

class HeatDemand {
 public:
  static constexpr uint32_t kStartDelayMs = 60000;  // sensors and rooms settle after a start

  HeatDemandConfig cfg;
  uint32_t revision = 0;  // configuration changes (Home Assistant discovery)
  // runtime
  bool active = false;
  Millis changedMs = 0;
  Millis lastDemandMs = 0;
  Millis lastSendMs = 0;
  bool sentOnce = false;
  uint32_t sends = 0;
  std::string lastSent;  // "on" / "off" / "fallback" / error text
  std::vector<std::string> requesting;  // rooms that currently need heat

  struct Step {
    bool changed = false;
    bool send = false;
  };
  // Called once per second. `startMs` is the start of the controller.
  Step update(bool anyDemand, std::vector<std::string> rooms, Millis now, Millis startMs);
  void reset();

  void toJson(JsonObject o) const;
  std::string fromJson(JsonObjectConst o);  // validated, all or nothing
  void statusJson(JsonObject o, Millis now) const;
};

}  // namespace hc
