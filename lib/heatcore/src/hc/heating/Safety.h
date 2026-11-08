#pragma once
// Safety supervisor. It is the last stage before the outputs and cannot be
// configured or bypassed by automation rules, RF devices or MQTT/Home
// Assistant; its configuration is only writable through the local web API.
#include <ArduinoJson.h>

#include <cstdint>
#include <string>

#include "hc/core/Alarms.h"
#include "hc/core/Clock.h"
#include "hc/core/Event.h"
#include "hc/heating/Heating.h"
#include "hc/io/IoManager.h"
#include "hc/sensors/SensorManager.h"

namespace hc {

struct SafetyConfig {
  float minSetpoint = 5.f;
  float maxSetpoint = 28.f;
  float maxRoomTemp = 30.f;           // room over-temperature cut
  std::string flowSensorId;           // empty: first sensor with role "flow"
  float maxFlowTemp = 55.f;           // supply over-temperature cut (all circuits)
  float flowHysteresis = 5.f;
  bool flowSensorRequired = false;    // missing flow sensor -> all circuits closed
  uint32_t maxManualOverrideSec = 7200;  // web manual output override
  uint32_t haMaxOutputSec = 3600;        // Home Assistant / automation timed outputs
  uint16_t minSwitchIntervalSec = 10;    // general outputs and manual overrides
  uint16_t maxSwitchesPerHour = 60;
  uint16_t watchdogSec = 30;             // control loop stall -> outputs safe
  SensorFailMode sensorFailMode = SensorFailMode::Off;
  uint8_t sensorFailDutyPct = 20;
  uint16_t sensorFailCycleMin = 20;
  // Power supply of the valve outputs. Thermal actuators draw several times
  // their rated power while cold (PTC heater): Danfoss Thermot 088H3216 /
  // TWA-A 24 V: 2 W running, inrush up to 350 mA, stroke about 3 min. The
  // duration of the inrush is not specified: assume the whole stroke. With a rated current set, valves are switched on only
  // while the supply has headroom; the rest wait (staggered start).
  float psuVoltage = 24.f;        // 12 or 24 V DC
  float psuCurrentA = 0.f;        // rated output current, 0 = not limited
  uint8_t psuUsablePct = 80;      // usable share of the rating (headroom)
  float psuBaseLoadA = 0.25f;     // board and other loads on the same supply
  float valveInrushA = 0.35f;     // per actuator while starting
  float valveHoldW = 2.0f;        // per actuator when warm (running power)
  uint16_t valveInrushSec = 180;  // duration of the starting phase
  float psuBudgetA() const { return psuCurrentA * static_cast<float>(psuUsablePct) / 100.f; }
  float valveHoldA() const { return psuVoltage > 0.f ? valveHoldW / psuVoltage : 0.f; }
  // current reserved when a valve is admitted: the larger of start and hold
  float valveStartA() const { return valveInrushA > valveHoldA() ? valveInrushA : valveHoldA(); }
};

class SafetySupervisor {
 public:
  SafetySupervisor(AlarmManager& alarms, EventBus& bus, const IClock& clock)
      : alarms_(alarms), bus_(bus), clock_(clock) {}

  SafetyConfig cfg;

  void evaluate(HeatingManager& heating, const SensorManager& sensors, Millis now);
  // Final output level. `safeLevel` closes the valve / switches the load off.
  bool filterOutput(int idx, bool requested, bool isCircuit, bool safeLevel, bool roomOverTemp, const OutputState& st,
                    Millis now, std::string& reason) const;

  float clampSetpoint(float v) const;
  uint32_t clampOverrideSec(uint32_t sec, EventSource src) const;
  bool flowTripped() const { return flowTrip_; }
  bool flowValid() const { return flowValid_; }
  float flowTemp() const { return flowTemp_; }
  uint32_t trips() const { return trips_; }

  // Power budget (only valve outputs count; 0 A rating = no limit)
  bool powerLimited() const { return cfg.psuCurrentA > 0.f; }
  float valveLoadA(const OutputState& st, Millis now) const;
  void setPowerStatus(float loadA, uint8_t waiting) {
    powerLoadA_ = loadA;
    powerWaiting_ = waiting;
  }
  float powerLoadA() const { return powerLoadA_; }
  uint8_t powerWaiting() const { return powerWaiting_; }

  void toJson(JsonObject o, bool runtime) const;
  std::string fromJson(JsonObjectConst o);

 private:
  AlarmManager& alarms_;
  EventBus& bus_;
  const IClock& clock_;
  bool flowTrip_ = false;
  bool flowValid_ = false;
  float flowTemp_ = 0.f;
  uint32_t trips_ = 0;
  float powerLoadA_ = 0.f;
  uint8_t powerWaiting_ = 0;
};

}  // namespace hc
