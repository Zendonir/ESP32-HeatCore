#include "hc/heating/HeatDemand.h"

#include <cmath>

namespace hc {

void HeatDemand::reset() {
  active = false;
  changedMs = lastDemandMs = lastSendMs = 0;
  sentOnce = false;
  requesting.clear();
}

HeatDemand::Step HeatDemand::update(bool anyDemand, std::vector<std::string> rooms, Millis now, Millis startMs) {
  Step s;
  requesting = std::move(rooms);
  if (!cfg.enabled) return s;
  if (elapsed(now, startMs) < kStartDelayMs) return s;  // rooms have no valid demand yet
  if (anyDemand) lastDemandMs = now ? now : 1;
  if (!active && anyDemand && (!changedMs || elapsed(now, changedMs) >= cfg.minOffSec * 1000u)) {
    active = true;
    changedMs = now ? now : 1;
    s.changed = true;
  } else if (active && !anyDemand && elapsed(now, lastDemandMs) >= cfg.runOnSec * 1000u &&
             elapsed(now, changedMs) >= cfg.minOnSec * 1000u) {
    active = false;
    changedMs = now ? now : 1;
    s.changed = true;
  }
  s.send = s.changed || !sentOnce || (cfg.repeatSec && elapsed(now, lastSendMs) >= cfg.repeatSec * 1000u);
  return s;
}

void HeatDemand::toJson(JsonObject o) const {
  o["enabled"] = cfg.enabled;
  JsonArray r = o["rooms"].to<JsonArray>();
  for (auto& x : cfg.rooms) r.add(x);
  o["runOnSec"] = cfg.runOnSec;
  o["minOnSec"] = cfg.minOnSec;
  o["minOffSec"] = cfg.minOffSec;
  o["repeatSec"] = cfg.repeatSec;
  o["output"] = cfg.output;
  o["target"] = cfg.target;
  o["topic"] = cfg.topic;
  o["payloadOn"] = cfg.payloadOn;
  o["payloadOff"] = cfg.payloadOff;
  o["payloadFallback"] = cfg.payloadFallback;
  o["retain"] = cfg.retain;
  o["supply"] = cfg.supply;
  o["supplySensor"] = cfg.supplySensor;
  o["supplyMinTemp"] = cfg.supplyMinTemp;
  o["supplyDelaySec"] = cfg.supplyDelaySec;
}

std::string HeatDemand::fromJson(JsonObjectConst o) {
  HeatDemandConfig n;
  n.enabled = o["enabled"] | false;
  for (const char* r : o["rooms"].as<JsonArrayConst>())
    if (r && *r) n.rooms.push_back(r);
  n.runOnSec = o["runOnSec"] | n.runOnSec;
  n.minOnSec = o["minOnSec"] | n.minOnSec;
  n.minOffSec = o["minOffSec"] | n.minOffSec;
  n.repeatSec = o["repeatSec"] | n.repeatSec;
  n.output = o["output"] | 0;
  n.target = o["target"] | "mqtt";
  n.topic = o["topic"] | "";
  n.payloadOn = o["payloadOn"] | "";
  n.payloadOff = o["payloadOff"] | "";
  n.payloadFallback = o["payloadFallback"] | "";
  n.retain = o["retain"] | false;
  // heat supply: kept when a caller does not send it
  n.supply = o["supply"] | cfg.supply.c_str();
  n.supplySensor = o["supplySensor"] | cfg.supplySensor.c_str();
  n.supplyMinTemp = o["supplyMinTemp"] | cfg.supplyMinTemp;
  n.supplyDelaySec = o["supplyDelaySec"] | cfg.supplyDelaySec;
  if (n.runOnSec > 7200) return "Nachlauf max. 120 min";
  if (n.minOnSec > 7200 || n.minOffSec > 7200) return "Mindestlauf-/pausenzeit max. 120 min";
  if (n.repeatSec != 0 && (n.repeatSec < 30 || n.repeatSec > 3600)) return "Wiederholen 0 oder 30..3600 s";
  if (n.output < 0 || n.output > 16) return "Ausgang 0..16";
  if (n.target != "mqtt" && n.target != "emsesp") return "Ziel: mqtt oder emsesp";
  bool mqtt = n.target == "mqtt";
  if (n.topic.size() > 120) return "MQTT-Thema max. 120 Zeichen";
  for (char c : n.topic)
    if (c == '+' || c == '#' || c == ' ') return "MQTT-Thema ohne Leerzeichen und Platzhalter (+, #)";
  if (mqtt && !n.topic.empty() && (n.payloadOn.empty() || n.payloadOff.empty()))
    return "Nachricht fuer Ein und Aus angeben";
  if (n.payloadOn.size() > 200 || n.payloadOff.size() > 200 || n.payloadFallback.size() > 200)
    return "Nachrichten max. 200 Zeichen";
  if (n.enabled && mqtt && n.topic.empty() && n.output == 0) return "MQTT-Thema oder Ausgang angeben";
  if (n.supply != "off" && n.supply != "emsesp" && n.supply != "sensor") return "Waermeangebot: off, emsesp oder sensor";
  if (n.supply == "sensor" && n.supplySensor.empty()) return "Fuehler fuer das Waermeangebot waehlen";
  if (!(n.supplyMinTemp >= 15.f && n.supplyMinTemp <= 70.f)) return "Mindest-Vorlauf 15..70 C";
  if (n.supplyDelaySec > 3600) return "Verzoegerung max. 60 min";
  if (n.supply != cfg.supply || n.supplySensor != cfg.supplySensor) {
    supplyOk = true;  // new source: decide again from scratch
    supplyLowMs = 0;
  }
  bool wasEnabled = cfg.enabled;
  cfg = std::move(n);
  ++revision;
  if (!cfg.enabled || !wasEnabled) reset();
  return std::string();
}

void HeatDemand::statusJson(JsonObject o, Millis now) const {
  o["enabled"] = cfg.enabled;
  o["active"] = active;
  o["sinceSec"] = changedMs ? elapsed(now, changedMs) / 1000u : 0u;
  JsonArray r = o["requesting"].to<JsonArray>();
  for (auto& x : requesting) r.add(x);
  if (active && requesting.empty() && lastDemandMs) {
    uint32_t left = cfg.runOnSec * 1000u > elapsed(now, lastDemandMs) ? cfg.runOnSec - elapsed(now, lastDemandMs) / 1000u : 0u;
    o["runOnLeftSec"] = left;
  }
  o["sends"] = sends;
  o["lastSent"] = lastSent;
  o["lastSentAgoSec"] = lastSendMs ? elapsed(now, lastSendMs) / 1000u : 0u;
  JsonObject s = o["supply"].to<JsonObject>();
  s["mode"] = cfg.supply;
  s["ok"] = supplyOk;
  if (supplyKnown) s["value"] = std::round(supplyValue * 10.f) / 10.f;
  s["text"] = supplyText;
  if (supplyLowMs) s["lowSec"] = elapsed(now, supplyLowMs) / 1000u;
}

}  // namespace hc
