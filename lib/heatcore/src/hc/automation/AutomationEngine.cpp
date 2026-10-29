#include "hc/automation/AutomationEngine.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "hc/core/Log.h"
#include "hc/heating/Heating.h"

namespace hc {

namespace {
constexpr const char* TAG = "automation";
constexpr size_t kLogSize = 60;

bool refMatch(const std::string& want, const char* have) {
  if (want.empty() || want == "*") return true;
  return want == have;
}

// RF device-level reference "rf:5" also matches button refs "rf:5/2".
bool inputRefMatch(const std::string& want, const char* have) {
  if (want.empty() || want == "*") return true;
  if (want == have) return true;
  size_t n = want.size();
  return want.rfind("rf:", 0) == 0 && want.find('/') == std::string::npos && std::strncmp(have, want.c_str(), n) == 0 &&
         have[n] == '/';
}

std::string fmt(float v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%.1f", v);
  return b;
}

// ---- static cycle analysis signatures
enum class SigKind { RoomSetpoint, RoomMode, RoomWindow, RoomPresence, RoomHeating, Valve, GlobalMode, Mqtt, Alarm };
struct Sig {
  SigKind kind;
  std::string ref;
};

bool sigRefMatch(const std::string& a, const std::string& b) { return a == "*" || b == "*" || a.empty() || b.empty() || a == b; }

std::vector<Sig> emittedBy(const Rule& r) {
  std::vector<Sig> s;
  for (auto& a : r.actions) {
    switch (a.type) {
      case ActionType::SetSetpoint:
      case ActionType::AdjustSetpoint:
        s.push_back({SigKind::RoomSetpoint, a.room});
        s.push_back({SigKind::RoomHeating, a.room});
        s.push_back({SigKind::Valve, "*"});
        break;
      case ActionType::DisplayedRoomAdjust:
        s.push_back({SigKind::RoomSetpoint, "*"});
        s.push_back({SigKind::RoomHeating, "*"});
        s.push_back({SigKind::Valve, "*"});
        break;
      case ActionType::SetRoomMode:
        s.push_back({SigKind::RoomMode, a.room});
        s.push_back({SigKind::RoomHeating, a.room});
        s.push_back({SigKind::Valve, "*"});
        break;
      case ActionType::ActivateRoomGroup:
        s.push_back({SigKind::RoomMode, "*"});
        s.push_back({SigKind::RoomHeating, "*"});
        s.push_back({SigKind::Valve, "*"});
        break;
      case ActionType::SetGlobalMode:
        s.push_back({SigKind::GlobalMode, a.text});
        s.push_back({SigKind::RoomHeating, "*"});
        s.push_back({SigKind::Valve, "*"});
        break;
      case ActionType::WindowState:
        s.push_back({SigKind::RoomWindow, a.room});
        s.push_back({SigKind::RoomHeating, a.room});
        s.push_back({SigKind::Valve, "*"});
        break;
      case ActionType::Presence:
        s.push_back({SigKind::RoomPresence, a.room.empty() ? "*" : a.room});
        s.push_back({SigKind::RoomHeating, a.room.empty() ? "*" : a.room});
        break;
      case ActionType::HeatDemand:
        s.push_back({SigKind::RoomHeating, a.room});
        s.push_back({SigKind::Valve, "*"});
        break;
      case ActionType::MqttPublish: s.push_back({SigKind::Mqtt, a.target}); break;
      case ActionType::RaiseAlarm: s.push_back({SigKind::Alarm, a.target}); break;
      default: break;
    }
  }
  return s;
}

bool triggeredBy(const Rule& r, const Sig& sig) {
  for (auto& t : r.triggers) {
    switch (t.type) {
      case TriggerType::RoomState: {
        SigKind k;
        if (t.state == "setpoint") k = SigKind::RoomSetpoint;
        else if (t.state == "mode") k = SigKind::RoomMode;
        else if (t.state == "window_open" || t.state == "window_closed") k = SigKind::RoomWindow;
        else if (t.state == "presence_on" || t.state == "presence_off") k = SigKind::RoomPresence;
        else k = SigKind::RoomHeating;
        if (sig.kind == k && sigRefMatch(t.ref, sig.ref)) return true;
        break;
      }
      case TriggerType::ValveState:
        if (sig.kind == SigKind::Valve) return true;
        break;
      case TriggerType::GlobalMode:
        if (sig.kind == SigKind::GlobalMode && sigRefMatch(t.state, sig.ref)) return true;
        break;
      case TriggerType::Mqtt:
        if (sig.kind == SigKind::Mqtt && t.ref == sig.ref) return true;
        break;
      case TriggerType::Alarm:
        if (sig.kind == SigKind::Alarm && (t.ref.empty() || t.ref == "*" || sig.ref.rfind(t.ref, 0) == 0)) return true;
        break;
      default: break;
    }
  }
  return false;
}
}  // namespace

const Rule* AutomationEngine::rule(uint16_t id) const {
  for (auto& r : rules_)
    if (r.id == id) return &r;
  return nullptr;
}

std::string AutomationEngine::findCycle(const Rule& candidate) const {
  // graph over enabled rules plus the candidate
  std::vector<const Rule*> nodes;
  nodes.push_back(&candidate);
  for (auto& r : rules_)
    if (r.enabled && r.id != candidate.id) nodes.push_back(&r);
  size_t n = nodes.size();
  std::vector<std::vector<size_t>> adj(n);
  for (size_t i = 0; i < n; ++i) {
    auto sigs = emittedBy(*nodes[i]);
    for (size_t j = 0; j < n; ++j)
      for (auto& s : sigs)
        if (triggeredBy(*nodes[j], s)) {
          adj[i].push_back(j);
          break;
        }
  }
  // DFS from the candidate back to itself
  std::vector<size_t> path;
  std::vector<bool> seen(n, false);
  std::string result;
  std::function<bool(size_t)> dfs = [&](size_t u) -> bool {
    for (size_t v : adj[u]) {
      if (v == 0) {
        path.push_back(v);
        return true;
      }
      if (seen[v]) continue;
      seen[v] = true;
      path.push_back(v);
      if (dfs(v)) return true;
      path.pop_back();
    }
    return false;
  };
  path.push_back(0);
  if (!dfs(0)) return std::string();
  for (size_t i = 0; i < path.size(); ++i) {
    if (i) result += " -> ";
    result += nodes[path[i]]->name;
  }
  return result;
}

std::string AutomationEngine::validate(const Rule& r) const {
  if (r.name.empty() || r.name.size() > 40) return "Name 1..40 Zeichen";
  if (r.triggers.empty()) return "Mindestens ein Trigger erforderlich";
  if (r.actions.empty()) return "Mindestens eine Aktion erforderlich";
  if (r.triggers.size() > 8 || r.conditions.size() > 12 || r.actions.size() > 10) return "Zu viele Elemente";
  if (r.priority < 0 || r.priority > 100) return "Prioritaet 0..100";
  if (r.cooldownSec > 86400) return "Sperrzeit max. 86400 s";
  if (r.maxPerHour < 1 || r.maxPerHour > 3600) return "Ausfuehrungen/h 1..3600";
  for (auto& t : r.triggers) {
    switch (t.type) {
      case TriggerType::Input:
        if (t.ref != "*" && !ctx_.refExists("input", t.ref)) return "Eingang unbekannt: " + t.ref;
        break;
      case TriggerType::TempThreshold:
      case TriggerType::TempChange:
        if (t.ref.rfind("sensor:", 0) == 0 ? !ctx_.refExists("sensor", t.ref.substr(7)) : !ctx_.refExists("room", t.ref))
          return "Temperaturquelle unbekannt: " + t.ref;
        if (t.type == TriggerType::TempThreshold && (t.hysteresis < 0.1f || t.hysteresis > 5.f))
          return "Trigger-Hysterese 0.1..5 K";
        if (t.type == TriggerType::TempChange && (std::fabs(t.value) < 0.1f || t.windowSec < 60 || t.windowSec > 86400))
          return "Temperaturaenderung: |Delta| >= 0.1 K, Zeitfenster 60..86400 s";
        break;
      case TriggerType::RoomState: {
        static const char* const states[] = {"setpoint", "mode", "window_open", "window_closed", "heating_on",
                                             "heating_off", "presence_on", "presence_off"};
        bool ok = false;
        for (auto s : states) ok |= t.state == s;
        if (!ok) return "Raumzustand unbekannt: " + t.state;
        if (t.ref != "*" && !ctx_.refExists("room", t.ref)) return "Raum unbekannt: " + t.ref;
        break;
      }
      case TriggerType::ValveState:
        if (t.index < 0 || t.index > 16) return "Heizkreis 0 (alle) ..16";
        if (t.state != "open" && t.state != "closed" && t.state != "fault") return "Ventilzustand open/closed/fault";
        break;
      case TriggerType::Mqtt:
        if (t.ref.empty() || t.ref.size() > 23 || t.ref.find_first_of("#+") != std::string::npos)
          return "MQTT-Thema 1..23 Zeichen ohne Platzhalter";
        break;
      case TriggerType::SensorFault:
        if (t.ref != "*" && !ctx_.refExists("sensor", t.ref)) return "Sensor unbekannt: " + t.ref;
        break;
      case TriggerType::ModelEvent:
        if (t.ref != "*" && !ctx_.refExists("room", t.ref)) return "Raum unbekannt: " + t.ref;
        break;
      case TriggerType::GlobalMode: {
        GlobalMode m;
        if (t.state != "*" && !globalModeFromName(t.state.c_str(), m)) return "Betriebsart unbekannt: " + t.state;
        break;
      }
      case TriggerType::Time:
        if (t.days == 0) return "Mindestens ein Wochentag";
        break;
      default: break;
    }
  }
  for (auto& c : r.conditions) {
    switch (c.type) {
      case ConditionType::Temperature:
        if (c.ref.rfind("sensor:", 0) == 0 ? !ctx_.refExists("sensor", c.ref.substr(7)) : !ctx_.refExists("room", c.ref))
          return "Temperaturquelle unbekannt: " + c.ref;
        break;
      case ConditionType::Setpoint:
      case ConditionType::RoomMode:
      case ConditionType::Window:
        if (!ctx_.refExists("room", c.ref)) return "Raum unbekannt: " + c.ref;
        break;
      case ConditionType::Presence:
        if (!c.ref.empty() && !ctx_.refExists("room", c.ref)) return "Raum unbekannt: " + c.ref;
        break;
      case ConditionType::SensorAvailable:
        if (!ctx_.refExists("sensor", c.ref)) return "Sensor unbekannt: " + c.ref;
        break;
      case ConditionType::InputState:
        if (!ctx_.refExists("input", c.ref)) return "Eingang unbekannt: " + c.ref;
        break;
      case ConditionType::OutputState:
        if (c.index < 1 || c.index > 16) return "Ausgang 1..16";
        break;
      case ConditionType::RoomGroup:
        if (!ctx_.refExists("group", c.ref)) return "Raumgruppe unbekannt: " + c.ref;
        break;
      case ConditionType::Weekday:
        if (c.days == 0) return "Mindestens ein Wochentag";
        break;
      default: break;
    }
  }
  for (auto& a : r.actions) {
    std::string e = ctx_.validateAction(a);
    if (!e.empty()) return describeAction(a) + ": " + e;
  }
  std::string cyc = findCycle(r);
  if (!cyc.empty()) return "Moegliche Endlosschleife: " + cyc;
  return std::string();
}

std::string AutomationEngine::upsert(Rule r, uint16_t* id, std::string* validationError) {
  std::string err = validate(r);
  r.valid = err.empty();
  r.validationError = err;
  if (!err.empty()) r.enabled = false;  // never activate an invalid rule
  if (validationError) *validationError = err;
  if (r.id == 0) {
    if (rules_.size() >= kMaxRules) return "Maximale Regelanzahl erreicht";
    uint16_t next = 1;
    for (auto& x : rules_) next = std::max<uint16_t>(next, x.id + 1);
    r.id = next;
    rules_.push_back(std::move(r));
    if (id) *id = rules_.back().id;
  } else {
    auto it = std::find_if(rules_.begin(), rules_.end(), [&](const Rule& x) { return x.id == r.id; });
    if (it == rules_.end()) return "Regel nicht gefunden";
    r.execCount = it->execCount;
    r.blockedCount = it->blockedCount;
    r.lastExecEpoch = it->lastExecEpoch;
    r.lastExecMs = it->lastExecMs;
    r.executedOnce = it->executedOnce;
    r.lastResult = it->lastResult;
    if (id) *id = r.id;
    *it = std::move(r);
  }
  ++revision_;
  return std::string();
}

bool AutomationEngine::remove(uint16_t id) {
  auto it = std::remove_if(rules_.begin(), rules_.end(), [&](const Rule& r) { return r.id == id; });
  if (it == rules_.end()) return false;
  rules_.erase(it, rules_.end());
  ++revision_;
  return true;
}

bool AutomationEngine::setEnabled(uint16_t id, bool enabled, std::string* err) {
  for (auto& r : rules_) {
    if (r.id != id) continue;
    if (enabled) {
      Rule probe = r;
      probe.enabled = true;
      std::string e = validate(probe);
      r.valid = e.empty();
      r.validationError = e;
      if (!e.empty()) {
        if (err) *err = e;
        return false;
      }
    }
    r.enabled = enabled;
    ++revision_;
    return true;
  }
  if (err) *err = "Regel nicht gefunden";
  return false;
}

void AutomationEngine::revalidateAll() {
  for (auto& r : rules_) {
    std::string e = validate(r);
    r.valid = e.empty();
    r.validationError = e;
    if (!e.empty() && r.enabled) {
      r.enabled = false;
      HC_LOGW(TAG, "rule '%s' disabled: %s", r.name.c_str(), e.c_str());
    }
  }
}

bool AutomationEngine::triggerMatches(const Trigger& t, const Event& e) const {
  switch (t.type) {
    case TriggerType::Input:
      return e.type == EventType::InputGesture && inputRefMatch(t.ref, e.ref) &&
             (t.gesture == Gesture::None || t.gesture == e.gesture);
    case TriggerType::RoomState: {
      if (!refMatch(t.ref, e.ref)) return false;
      switch (e.type) {
        case EventType::RoomSetpointChanged: return t.state == "setpoint";
        case EventType::RoomModeChanged: return t.state == "mode";
        case EventType::RoomWindowChanged: return t.state == (e.value != 0.f ? "window_open" : "window_closed");
        case EventType::RoomHeatingChanged: return t.state == (e.value != 0.f ? "heating_on" : "heating_off");
        case EventType::RoomPresenceChanged: return t.state == (e.value != 0.f ? "presence_on" : "presence_off");
        default: return false;
      }
    }
    case TriggerType::ValveState:
      if (t.index != 0 && t.index != e.a) return false;
      if (e.type == EventType::ValveChanged) return t.state == (e.value != 0.f ? "open" : "closed");
      return e.type == EventType::CircuitFault && t.state == "fault";
    case TriggerType::Time:
      return e.type == EventType::TimeMinute && static_cast<int>(e.value) == t.time && e.a >= 0 && e.a < 7 &&
             (t.days & (1u << e.a));
    case TriggerType::Mqtt:
      return e.type == EventType::MqttMessage && t.ref == e.ref && (t.text.empty() || t.text == e.text);
    case TriggerType::SensorFault: return e.type == EventType::SensorFault && refMatch(t.ref, e.ref);
    case TriggerType::ModelEvent:
      return e.type == EventType::ModelEvent && refMatch(t.ref, e.ref) && (t.state.empty() || t.state == e.text);
    case TriggerType::Manual: return e.type == EventType::ManualTrigger;
    case TriggerType::GlobalMode: return e.type == EventType::GlobalModeChanged && refMatch(t.state, e.ref);
    case TriggerType::Alarm:
      return e.type == EventType::Alarm && (t.ref.empty() || t.ref == "*" || std::string(e.ref).rfind(t.ref, 0) == 0);
    default: return false;  // polled triggers
  }
}

bool AutomationEngine::conditionHolds(const Condition& c) const {
  bool r = false;
  switch (c.type) {
    case ConditionType::Temperature: {
      float v;
      r = ctx_.temperature(c.ref, v) && compare(v, c.op, c.value);
      break;
    }
    case ConditionType::Setpoint: {
      float v;
      r = ctx_.setpoint(c.ref, v) && compare(v, c.op, c.value);
      break;
    }
    case ConditionType::TimeRange: {
      LocalTime lt = ctx_.localTime();
      if (!lt.valid) {
        r = false;
        break;
      }
      int m = lt.minuteOfDay();
      r = c.from <= c.to ? (m >= c.from && m < c.to) : (m >= c.from || m < c.to);
      break;
    }
    case ConditionType::Weekday: {
      LocalTime lt = ctx_.localTime();
      r = lt.valid && (c.days & (1u << lt.weekday));
      break;
    }
    case ConditionType::GlobalMode: r = ctx_.globalMode() == c.text; break;
    case ConditionType::RoomMode: r = ctx_.roomMode(c.ref) == c.text; break;
    case ConditionType::Presence: {
      int p = ctx_.presence(c.ref);
      r = p >= 0 && (p == 1) == (c.value != 0.f);
      break;
    }
    case ConditionType::Window: r = ctx_.windowOpen(c.ref) == (c.value != 0.f); break;
    case ConditionType::SensorAvailable: r = ctx_.sensorAvailable(c.ref) == (c.value != 0.f); break;
    case ConditionType::InputState: {
      int s = ctx_.inputState(c.ref);
      r = s >= 0 && (s == 1) == (c.value != 0.f);
      break;
    }
    case ConditionType::OutputState: r = ctx_.outputOn(c.index) == (c.value != 0.f); break;
    case ConditionType::RoomGroup: r = ctx_.groupState(c.ref, c.text); break;
    default: r = false;
  }
  return c.negate ? !r : r;
}

bool AutomationEngine::conditionsHold(const Rule& r) const {
  for (auto& c : r.conditions)
    if (!conditionHolds(c)) return false;
  return true;
}

bool AutomationEngine::paused() const { return pausedFlag_ && !reached(ctx_.now(), pausedUntil_); }

bool AutomationEngine::rateAllowed(Rule& r, Millis now) {
  if (r.executedOnce && elapsed(now, r.lastExecMs) < r.cooldownSec * 1000u) return false;
  if (elapsed(now, r.hourStart) >= 3600u * 1000u) {
    r.hourStart = now;
    r.execThisHour = 0;
  }
  return r.execThisHour < r.maxPerHour;
}

void AutomationEngine::onEvent(const Event& e) {
  if (rules_.empty()) return;
  Millis now = ctx_.now();
  if (pausedFlag_) {
    if (!reached(now, pausedUntil_)) return;
    pausedFlag_ = false;
    ctx_.systemAlarm("automation.storm", "", false);
  }
  if (e.depth >= maxDepth) {
    ++stats_.blockedDepth;
    HC_LOGW(TAG, "event %s at depth %u not processed (loop protection)", eventTypeName(e.type), e.depth);
    return;
  }
  std::vector<Rule*> matched;
  for (auto& r : rules_) {
    if (!r.enabled || !r.valid) continue;
    bool hit = false;
    for (auto& t : r.triggers)
      if (triggerMatches(t, e)) hit = true;
    if (e.type == EventType::ManualTrigger && e.a != r.id) hit = false;
    if (!hit) continue;
    if (e.ruleId == r.id) {
      ++stats_.blockedSelf;
      continue;
    }
    if (!conditionsHold(r)) continue;
    matched.push_back(&r);
  }
  if (!matched.empty()) runRules(matched, e);
}

void AutomationEngine::runRules(std::vector<Rule*>& matched, const Event& e) {
  Millis now = ctx_.now();
  std::stable_sort(matched.begin(), matched.end(), [](const Rule* a, const Rule* b) { return a->priority > b->priority; });
  std::vector<std::pair<std::string, const Rule*>> claimed;
  std::string trig = std::string(eventTypeName(e.type)) + (e.ref[0] ? std::string(" ") + e.ref : std::string());
  if (e.type == EventType::InputGesture) trig += std::string(" ") + gestureName(e.gesture);
  for (Rule* r : matched) {
    if (!rateAllowed(*r, now)) {
      ++stats_.blockedRate;
      ++r->blockedCount;
      r->lastResult = "gesperrt (Sperrzeit/Limit)";
      addLog(*r, trig, r->lastResult, false);
      continue;
    }
    // global storm guard
    while (!recentExec_.empty() && elapsed(now, recentExec_.front()) > 10000) recentExec_.pop_front();
    if (recentExec_.size() >= stormMaxExec) {
      ++stats_.storms;
      pausedFlag_ = true;
      pausedUntil_ = now + stormPauseSec * 1000u;
      ctx_.systemAlarm("automation.storm", "Zu viele Regelausfuehrungen - Automationen pausiert", true);
      HC_LOGE(TAG, "automation storm detected, pausing for %u s", stormPauseSec);
      return;
    }
    recentExec_.push_back(now);
    ActionOrigin origin{EventSource::Automation, static_cast<uint8_t>(e.depth + 1), r->id, r->testMode};
    std::string result;
    int ok = 0, skipped = 0, failed = 0;
    for (auto& a : r->actions) {
      std::string key = actionTargetKey(a);
      if (!key.empty()) {
        auto it = std::find_if(claimed.begin(), claimed.end(), [&](const auto& c) { return c.first == key; });
        if (it != claimed.end() && it->second != r) {
          ++skipped;
          ++stats_.conflicts;
          result += describeAction(a) + ": uebersteuert durch '" + it->second->name + "'; ";
          continue;
        }
        if (it == claimed.end()) claimed.push_back({key, r});
      }
      std::string err = ctx_.execute(a, origin);
      if (err.empty()) {
        ++ok;
      } else {
        ++failed;
        ++stats_.actionErrors;
        result += describeAction(a) + ": " + err + "; ";
      }
    }
    ++stats_.executions;
    ++r->execCount;
    ++r->execThisHour;
    r->lastExecMs = now;
    r->executedOnce = true;
    r->lastExecEpoch = ctx_.epoch();
    char head[48];
    std::snprintf(head, sizeof(head), "%d ok, %d uebersteuert, %d Fehler", ok, skipped, failed);
    r->lastResult = std::string(r->testMode ? "TESTMODUS (nicht ausgefuehrt): " : "") + head +
                    (result.empty() ? "" : " - " + result);
    addLog(*r, trig, r->lastResult, r->testMode);
    ++revision_;
  }
}

void AutomationEngine::tick() {
  if (rules_.empty() || paused()) return;
  Millis now = ctx_.now();
  for (auto& r : rules_) {
    if (!r.enabled || !r.valid) continue;
    for (auto& t : r.triggers) {
      if (t.type != TriggerType::TempThreshold && t.type != TriggerType::TempChange) continue;
      float v;
      if (!ctx_.temperature(t.ref, v)) continue;
      bool fire = false;
      if (t.type == TriggerType::TempThreshold) {
        bool beyond = t.above ? v >= t.value : v <= t.value;
        bool rearm = t.above ? v <= t.value - t.hysteresis : v >= t.value + t.hysteresis;
        if (!t.haveRef) {
          t.haveRef = true;
          t.armed = !beyond;  // no firing on the very first sample
        } else if (t.armed && beyond) {
          fire = true;
          t.armed = false;
        } else if (!t.armed && rearm) {
          t.armed = true;
        }
      } else {
        if (!t.haveRef) {
          t.haveRef = true;
          t.refValue = v;
          t.refMs = now;
        } else if ((t.value > 0 && v - t.refValue >= t.value) || (t.value < 0 && v - t.refValue <= t.value)) {
          fire = true;
          t.refValue = v;
          t.refMs = now;
        } else if (elapsed(now, t.refMs) > t.windowSec * 1000u) {
          t.refValue = v;
          t.refMs = now;
        }
      }
      if (fire && conditionsHold(r)) {
        Event e(EventType::SensorValue, EventSource::Sensor);
        e.ts = now;
        e.setRef(t.ref.c_str());
        e.value = v;
        std::vector<Rule*> one{&r};
        runRules(one, e);
        break;
      }
    }
  }
}

RuleTestReport AutomationEngine::test(uint16_t id, const Event* simulated) {
  for (auto& r : rules_)
    if (r.id == id) return testRule(r, simulated);
  RuleTestReport rep;
  rep.problems.push_back("Regel nicht gefunden");
  return rep;
}

RuleTestReport AutomationEngine::testRule(const Rule& r, const Event* simulated) {
  RuleTestReport rep;
  std::string v = validate(r);
  if (!v.empty()) rep.problems.push_back(v);
  if (simulated) {
    rep.triggerTested = true;
    for (auto& t : r.triggers) rep.triggerMatched |= triggerMatches(t, *simulated);
  }
  bool all = true;
  for (auto& c : r.conditions) {
    bool ok = conditionHolds(c);
    all &= ok;
    rep.conditions.push_back({describeCondition(c), ok});
  }
  ActionOrigin origin{EventSource::Automation, 1, r.id, true};
  for (auto& a : r.actions) {
    std::string res = ctx_.execute(a, origin);
    rep.actions.push_back({describeAction(a), res.empty() ? "ok" : res});
  }
  rep.wouldRun = v.empty() && all && (!rep.triggerTested || rep.triggerMatched);
  Rule copy = r;
  addLog(copy, simulated ? eventTypeName(simulated->type) : "Test", rep.wouldRun ? "Test: wuerde ausfuehren" : "Test: wuerde nicht ausfuehren", true);
  return rep;
}

void AutomationEngine::addLog(const Rule& r, const std::string& trig, const std::string& result, bool dry) {
  AutomationLogEntry e;
  e.seq = ++logSeq_;
  e.epoch = ctx_.epoch();
  e.ms = ctx_.now();
  e.ruleId = r.id;
  e.rule = r.name;
  e.trigger = trig;
  e.result = result;
  e.dryRun = dry;
  if (log_.size() >= kLogSize) log_.pop_front();
  log_.push_back(std::move(e));
}

std::vector<AutomationLogEntry> AutomationEngine::log(uint32_t sinceSeq, size_t maxCount) const {
  std::vector<AutomationLogEntry> out;
  for (auto it = log_.rbegin(); it != log_.rend() && out.size() < maxCount; ++it) {
    if (it->seq <= sinceSeq) break;
    out.push_back(*it);
  }
  std::reverse(out.begin(), out.end());
  return out;
}

std::string AutomationEngine::describeTrigger(const Trigger& t) const {
  switch (t.type) {
    case TriggerType::Input: return "Eingang " + t.ref + " " + (t.gesture == Gesture::None ? "beliebig" : gestureName(t.gesture));
    case TriggerType::TempThreshold: return t.ref + (t.above ? " >= " : " <= ") + fmt(t.value) + " C";
    case TriggerType::TempChange: return t.ref + " aendert sich um " + fmt(t.value) + " K";
    case TriggerType::RoomState: return "Raum " + t.ref + ": " + t.state;
    case TriggerType::ValveState: return "Heizkreis " + std::to_string(t.index) + ": " + t.state;
    case TriggerType::Time: return "Zeit " + formatHHMM(t.time);
    case TriggerType::Mqtt: return "MQTT " + t.ref;
    case TriggerType::SensorFault: return "Sensorfehler " + t.ref;
    case TriggerType::ModelEvent: return "Lernmodell " + t.ref + " " + t.state;
    case TriggerType::Manual: return "manuell";
    case TriggerType::GlobalMode: return "Betriebsart " + t.state;
    case TriggerType::Alarm: return "Alarm " + t.ref;
    default: return "?";
  }
}

std::string AutomationEngine::describeCondition(const Condition& c) const {
  std::string s;
  switch (c.type) {
    case ConditionType::Temperature: s = "Temperatur " + c.ref + " " + compareOpName(c.op) + " " + fmt(c.value); break;
    case ConditionType::Setpoint: s = "Sollwert " + c.ref + " " + compareOpName(c.op) + " " + fmt(c.value); break;
    case ConditionType::TimeRange: s = "Zeit " + formatHHMM(c.from) + "-" + formatHHMM(c.to); break;
    case ConditionType::Weekday: s = "Wochentag"; break;
    case ConditionType::GlobalMode: s = "Betriebsart = " + c.text; break;
    case ConditionType::RoomMode: s = "Raummodus " + c.ref + " = " + c.text; break;
    case ConditionType::Presence: s = "Anwesenheit " + (c.ref.empty() ? std::string("Haus") : c.ref); break;
    case ConditionType::Window: s = std::string("Fenster ") + c.ref + (c.value != 0.f ? " offen" : " zu"); break;
    case ConditionType::SensorAvailable: s = "Sensor " + c.ref + " verfuegbar"; break;
    case ConditionType::InputState: s = "Eingang " + c.ref + (c.value != 0.f ? " aktiv" : " inaktiv"); break;
    case ConditionType::OutputState: s = "Ausgang " + std::to_string(c.index) + (c.value != 0.f ? " an" : " aus"); break;
    case ConditionType::RoomGroup: s = "Gruppe " + c.ref + " " + c.text; break;
    default: s = "?";
  }
  return c.negate ? "NICHT " + s : s;
}

std::string AutomationEngine::describeAction(const Action& a) {
  switch (a.type) {
    case ActionType::SetSetpoint: return "Sollwert " + a.room + " = " + fmt(a.value);
    case ActionType::AdjustSetpoint: return "Sollwert " + a.room + (a.value >= 0 ? " +" : " ") + fmt(a.value);
    case ActionType::SetRoomMode: return "Raummodus " + a.room + " = " + a.text;
    case ActionType::SetGlobalMode: return "Betriebsart = " + a.text;
    case ActionType::ActivateRoomGroup: return "Gruppe " + a.target + " = " + a.text;
    case ActionType::SetOutput: return "Ausgang " + std::to_string(a.index) + (a.value != 0.f ? " an" : " aus");
    case ActionType::RfSend: return "Funkbefehl " + a.target;
    case ActionType::MqttPublish: return "MQTT " + a.target;
    case ActionType::Notify: return "Meldung";
    case ActionType::DisplayPage: return "Displayseite " + std::to_string(a.index);
    case ActionType::DisplayNext: return "Display weiter";
    case ActionType::DisplayPrev: return "Display zurueck";
    case ActionType::DisplayWake: return "Display wecken";
    case ActionType::DisplayRoomNext: return "Display naechster Raum";
    case ActionType::DisplayedRoomAdjust: return "Sollwert angezeigter Raum " + fmt(a.value);
    case ActionType::RaiseAlarm: return "Alarm " + a.target;
    case ActionType::ClearAlarm: return "Alarm loeschen " + a.target;
    case ActionType::WindowState: return std::string("Fenster ") + a.room + (a.value != 0.f ? " offen" : " zu");
    case ActionType::Presence: return "Anwesenheit " + a.room;
    case ActionType::HeatDemand: return "Waermeanforderung " + a.room;
    default: return actionTypeName(a.type);
  }
}

void AutomationEngine::toJson(JsonArray arr, bool runtime) const {
  for (auto& r : rules_) ruleToJson(r, arr.add<JsonObject>(), runtime);
}

std::string AutomationEngine::fromJson(JsonArrayConst arr) {
  std::vector<Rule> list;
  for (JsonObjectConst o : arr) {
    Rule r;
    std::string err;
    if (!ruleFromJson(r, o, err)) return err;
    if (r.id == 0) continue;
    list.push_back(std::move(r));
  }
  rules_ = std::move(list);
  revalidateAll();
  ++revision_;
  return std::string();
}

}  // namespace hc
