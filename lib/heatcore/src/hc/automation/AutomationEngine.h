#pragma once
// Central automation engine. All signal sources (wired inputs, RF433,
// sensors, schedule, MQTT, model, web) arrive as Events; actions are carried
// out through IAutomationContext, which applies the same safety limits as
// every other command path.
//
// Loop protection:
//  * causal depth: events produced by rule actions carry depth+1; events at
//    depth >= maxDepth are not processed by rules
//  * a rule never reacts to events it caused itself
//  * per-rule cooldown and executions-per-hour limit
//  * global storm guard (too many executions in 10 s pauses the engine)
//  * static cycle analysis when a rule is saved/enabled
#include <ArduinoJson.h>

#include <deque>
#include <string>
#include <vector>

#include "hc/automation/Rules.h"
#include "hc/core/Event.h"

namespace hc {

class IAutomationContext {
 public:
  virtual ~IAutomationContext() = default;
  virtual Millis now() const = 0;
  virtual int64_t epoch() const = 0;
  virtual LocalTime localTime() const = 0;
  // queries; ref for temperature is a room id or "sensor:<id>"
  virtual bool temperature(const std::string& ref, float& v) const = 0;
  virtual bool setpoint(const std::string& room, float& v) const = 0;
  virtual std::string globalMode() const = 0;
  virtual std::string roomMode(const std::string& room) const = 0;
  virtual int presence(const std::string& room) const = 0;  // -1 unknown
  virtual bool windowOpen(const std::string& room) const = 0;
  virtual bool sensorAvailable(const std::string& id) const = 0;
  virtual int inputState(const std::string& ref) const = 0;  // -1 unknown
  virtual bool outputOn(int output) const = 0;
  virtual bool groupState(const std::string& group, const std::string& state) const = 0;
  // validation: kind = room | sensor | input | group | circuit
  virtual bool refExists(const std::string& kind, const std::string& ref) const = 0;
  virtual std::string validateAction(const Action& a) const = 0;
  // execution; returns "" on success, otherwise the reason. With
  // origin.dryRun the context must not change anything.
  virtual std::string execute(const Action& a, const ActionOrigin& origin) = 0;
  virtual void systemAlarm(const std::string& code, const std::string& message, bool active) = 0;
};

struct AutomationLogEntry {
  uint32_t seq = 0;
  int64_t epoch = 0;
  Millis ms = 0;
  uint16_t ruleId = 0;
  std::string rule;
  std::string trigger;
  std::string result;
  bool dryRun = false;
};

struct RuleTestReport {
  bool triggerTested = false;
  bool triggerMatched = false;
  std::vector<std::pair<std::string, bool>> conditions;
  std::vector<std::pair<std::string, std::string>> actions;  // description, result
  std::vector<std::string> problems;
  bool wouldRun = false;
};

struct AutomationStats {
  uint32_t executions = 0;
  uint32_t blockedDepth = 0;
  uint32_t blockedSelf = 0;
  uint32_t blockedRate = 0;
  uint32_t conflicts = 0;
  uint32_t storms = 0;
  uint32_t actionErrors = 0;
};

class AutomationEngine {
 public:
  AutomationEngine(IAutomationContext& ctx, EventBus& bus) : ctx_(ctx), bus_(bus) {}

  static constexpr size_t kMaxRules = 64;
  uint8_t maxDepth = 3;
  uint16_t stormMaxExec = 25;  // per 10 s
  uint16_t stormPauseSec = 60;

  const std::vector<Rule>& rules() const { return rules_; }
  const Rule* rule(uint16_t id) const;
  // Stores the rule. If it fails validation it is stored disabled and the
  // error is returned in `validationError` (the call itself succeeds).
  std::string upsert(Rule r, uint16_t* id = nullptr, std::string* validationError = nullptr);
  bool remove(uint16_t id);
  bool setEnabled(uint16_t id, bool enabled, std::string* err);
  std::string validate(const Rule& r) const;
  void revalidateAll();

  void onEvent(const Event& e);
  void tick();
  RuleTestReport test(uint16_t id, const Event* simulated);
  RuleTestReport testRule(const Rule& r, const Event* simulated);

  bool paused() const;
  const AutomationStats& stats() const { return stats_; }
  std::vector<AutomationLogEntry> log(uint32_t sinceSeq, size_t maxCount) const;

  void toJson(JsonArray arr, bool runtime) const;
  std::string fromJson(JsonArrayConst arr);
  uint32_t revision() const { return revision_; }
  std::string describeTrigger(const Trigger& t) const;
  std::string describeCondition(const Condition& c) const;
  static std::string describeAction(const Action& a);

 private:
  bool triggerMatches(const Trigger& t, const Event& e) const;
  bool conditionHolds(const Condition& c) const;
  bool conditionsHold(const Rule& r) const;
  void runRules(std::vector<Rule*>& matched, const Event& e);
  bool rateAllowed(Rule& r, Millis now);
  void addLog(const Rule& r, const std::string& trig, const std::string& result, bool dry);
  std::string findCycle(const Rule& candidate) const;

  IAutomationContext& ctx_;
  EventBus& bus_;
  std::vector<Rule> rules_;
  AutomationStats stats_;
  std::deque<Millis> recentExec_;
  Millis pausedUntil_ = 0;
  bool pausedFlag_ = false;
  std::deque<AutomationLogEntry> log_;
  uint32_t logSeq_ = 0;
  uint32_t revision_ = 0;
};

}  // namespace hc
