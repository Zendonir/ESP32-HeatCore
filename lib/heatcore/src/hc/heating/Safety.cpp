#include "hc/heating/Safety.h"

#include <cmath>
#include <cstring>

namespace hc {

void SafetySupervisor::evaluate(HeatingManager& heating, const SensorManager& sensors, Millis now) {
  // ---- supply / flow temperature
  float flow = 0.f;
  bool have = false;
  if (!cfg.flowSensorId.empty()) {
    const Sensor* s = sensors.find(cfg.flowSensorId);
    if (s && s->available()) {
      flow = s->value;
      have = true;
    }
  } else {
    have = sensors.firstOfRole(SensorRole::Flow, flow);
  }
  flowValid_ = have;
  flowTemp_ = flow;
  bool trip = flowTrip_;
  if (have) {
    if (flow >= cfg.maxFlowTemp) trip = true;
    else if (flow <= cfg.maxFlowTemp - cfg.flowHysteresis) trip = false;
  } else {
    trip = cfg.flowSensorRequired;
  }
  if (trip != flowTrip_) {
    flowTrip_ = trip;
    if (trip) {
      ++trips_;
      alarms_.raise("safety.flow", have ? "Vorlauf-Uebertemperatur - alle Heizkreise geschlossen"
                                        : "Vorlauffuehler fehlt - alle Heizkreise geschlossen",
                    AlarmLevel::Critical, EventSource::Safety);
      Event e(EventType::SafetyTrip, EventSource::Safety);
      e.ts = now;
      e.setRef("flow");
      e.value = flow;
      bus_.post(e);
    } else {
      alarms_.clear("safety.flow");
    }
  }
  // ---- per room over-temperature and sensor loss
  for (auto& r : heating.rooms) {
    bool ot = r.overTemp;
    if (r.hasTemp) {
      if (r.temperature >= cfg.maxRoomTemp) ot = true;
      else if (r.temperature <= cfg.maxRoomTemp - 1.f) ot = false;
    }
    if (ot != r.overTemp) {
      r.overTemp = ot;
      std::string code = "safety.room." + r.id;
      if (ot) {
        ++trips_;
        alarms_.raise(code, "Raum-Uebertemperatur " + r.name, AlarmLevel::Critical, EventSource::Safety);
        Event e(EventType::SafetyTrip, EventSource::Safety);
        e.ts = now;
        e.setRef(r.id.c_str());
        e.value = r.temperature;
        bus_.post(e);
      } else {
        alarms_.clear(code);
      }
    }
    std::string sc = "room.sensor." + r.id;
    if (r.sensorFail && !alarms_.active(sc))
      alarms_.raise(sc,
                    "Kein Temperaturwert fuer " + r.name +
                        (cfg.sensorFailMode == SensorFailMode::Duty ? " - Notbetrieb Taktung" : " - Heizkreis geschlossen"),
                    AlarmLevel::Warning, EventSource::Safety);
    else if (!r.sensorFail && alarms_.active(sc))
      alarms_.clear(sc);
  }
}

bool SafetySupervisor::filterOutput(int idx, bool requested, bool isCircuit, bool safeLevel, bool roomOverTemp,
                                    const OutputState& st, Millis now, std::string& reason) const {
  (void)idx;
  reason.clear();
  if (isCircuit && (flowTrip_ || roomOverTemp)) {
    reason = flowTrip_ ? "Vorlaufschutz" : "Raum-Uebertemperatur";
    return safeLevel;
  }
  if (requested == st.level || !st.everChanged) return requested;
  // Moving to the safe level is always allowed immediately.
  if (requested == safeLevel) return requested;
  if (!isCircuit || st.manualActive) {
    if (st.everChanged && elapsed(now, st.lastChangeMs) < cfg.minSwitchIntervalSec * 1000u) {
      reason = "Mindestschaltabstand";
      return st.level;
    }
    if (st.switchesThisHour >= cfg.maxSwitchesPerHour && elapsed(now, st.hourStart) < 3600u * 1000u) {
      reason = "Schaltratenbegrenzung";
      return st.level;
    }
  }
  return requested;
}

float SafetySupervisor::clampSetpoint(float v) const {
  if (!std::isfinite(v)) return cfg.minSetpoint;
  return v < cfg.minSetpoint ? cfg.minSetpoint : (v > cfg.maxSetpoint ? cfg.maxSetpoint : v);
}

uint32_t SafetySupervisor::clampOverrideSec(uint32_t sec, EventSource src) const {
  uint32_t lim = (src == EventSource::Web) ? cfg.maxManualOverrideSec : cfg.haMaxOutputSec;
  if (sec == 0 || sec > lim) return lim;  // never unlimited
  return sec;
}

void SafetySupervisor::toJson(JsonObject o, bool runtime) const {
  o["minSetpoint"] = cfg.minSetpoint;
  o["maxSetpoint"] = cfg.maxSetpoint;
  o["maxRoomTemp"] = cfg.maxRoomTemp;
  o["flowSensorId"] = cfg.flowSensorId;
  o["maxFlowTemp"] = cfg.maxFlowTemp;
  o["flowHysteresis"] = cfg.flowHysteresis;
  o["flowSensorRequired"] = cfg.flowSensorRequired;
  o["maxManualOverrideSec"] = cfg.maxManualOverrideSec;
  o["haMaxOutputSec"] = cfg.haMaxOutputSec;
  o["minSwitchIntervalSec"] = cfg.minSwitchIntervalSec;
  o["maxSwitchesPerHour"] = cfg.maxSwitchesPerHour;
  o["watchdogSec"] = cfg.watchdogSec;
  o["sensorFailMode"] = cfg.sensorFailMode == SensorFailMode::Duty ? "duty" : "off";
  o["sensorFailDutyPct"] = cfg.sensorFailDutyPct;
  o["sensorFailCycleMin"] = cfg.sensorFailCycleMin;
  if (runtime) {
    o["flowTrip"] = flowTrip_;
    o["flowValid"] = flowValid_;
    if (flowValid_) o["flowTemp"] = flowTemp_;
    o["trips"] = trips_;
  }
}

std::string SafetySupervisor::fromJson(JsonObjectConst o) {
  SafetyConfig c = cfg;
  c.minSetpoint = o["minSetpoint"] | c.minSetpoint;
  c.maxSetpoint = o["maxSetpoint"] | c.maxSetpoint;
  c.maxRoomTemp = o["maxRoomTemp"] | c.maxRoomTemp;
  c.flowSensorId = o["flowSensorId"] | c.flowSensorId.c_str();
  c.maxFlowTemp = o["maxFlowTemp"] | c.maxFlowTemp;
  c.flowHysteresis = o["flowHysteresis"] | c.flowHysteresis;
  c.flowSensorRequired = o["flowSensorRequired"] | c.flowSensorRequired;
  c.maxManualOverrideSec = o["maxManualOverrideSec"] | c.maxManualOverrideSec;
  c.haMaxOutputSec = o["haMaxOutputSec"] | c.haMaxOutputSec;
  c.minSwitchIntervalSec = o["minSwitchIntervalSec"] | c.minSwitchIntervalSec;
  c.maxSwitchesPerHour = o["maxSwitchesPerHour"] | c.maxSwitchesPerHour;
  c.watchdogSec = o["watchdogSec"] | c.watchdogSec;
  const char* fm = o["sensorFailMode"] | (c.sensorFailMode == SensorFailMode::Duty ? "duty" : "off");
  c.sensorFailMode = std::strcmp(fm, "duty") == 0 ? SensorFailMode::Duty : SensorFailMode::Off;
  c.sensorFailDutyPct = o["sensorFailDutyPct"] | c.sensorFailDutyPct;
  c.sensorFailCycleMin = o["sensorFailCycleMin"] | c.sensorFailCycleMin;
  // hard limits that even the local configuration cannot exceed
  if (c.minSetpoint < 5.f || c.maxSetpoint > 30.f || c.minSetpoint >= c.maxSetpoint)
    return "Sollwertgrenzen: 5 <= min < max <= 30 C";
  if (c.maxRoomTemp < 24.f || c.maxRoomTemp > 35.f) return "Raum-Uebertemperatur 24..35 C";
  if (c.maxFlowTemp < 25.f || c.maxFlowTemp > 90.f) return "Vorlaufgrenze 25..90 C";
  if (c.flowHysteresis < 1.f || c.flowHysteresis > 20.f) return "Vorlaufhysterese 1..20 K";
  if (c.maxManualOverrideSec < 60 || c.maxManualOverrideSec > 8 * 3600) return "Handbetrieb 60 s..8 h";
  if (c.haMaxOutputSec < 60 || c.haMaxOutputSec > 4 * 3600) return "Fernschaltdauer 60 s..4 h";
  if (c.minSwitchIntervalSec < 1 || c.minSwitchIntervalSec > 600) return "Schaltabstand 1..600 s";
  if (c.maxSwitchesPerHour < 2 || c.maxSwitchesPerHour > 600) return "Schaltungen/h 2..600";
  if (c.watchdogSec < 5 || c.watchdogSec > 300) return "Watchdog 5..300 s";
  if (c.sensorFailDutyPct > 50) return "Notbetrieb-Taktung max. 50 %";
  if (c.sensorFailCycleMin < 5 || c.sensorFailCycleMin > 120) return "Taktzyklus 5..120 min";
  cfg = c;
  return std::string();
}

}  // namespace hc
