#pragma once
// Settings changes made by an AI assistant (MCP interface).
//
// Access levels (web UI, System -> KI / MCP):
//   off      MCP endpoint disabled
//   read     statistics, settings and model may be read
//   approve  changes become proposals; a person accepts or rejects them in
//            the web interface
//   direct   changes are applied immediately (still validated, rate limited)
//
// Only a small set of tuning parameters is reachable, each with tighter
// bounds than the web interface: room temperatures (comfort/eco/frost),
// hysteresis, model assistance, comfort schedules, valve minimum on/off
// times and the model advice limit. Safety limits, outputs, inputs, rules,
// RF, network, MQTT and passwords are never reachable. Every change is
// journaled with its previous values and can be reverted.
#include <ArduinoJson.h>

#include <cstdint>
#include <string>
#include <vector>

namespace hc {

class HeatCoreSystem;

enum class AiAccess : uint8_t { Off = 0, Read, Approve, Direct };
const char* aiAccessName(AiAccess a);
bool aiAccessFromName(const char* s, AiAccess& out);

struct AiConfig {
  AiAccess access = AiAccess::Off;
  std::string token;            // bearer token for the MCP endpoint
  uint8_t maxDirectPerDay = 10; // applied changes per 24 h in direct mode
};

struct AiChange {
  uint16_t id = 0;
  int64_t epoch = 0;
  std::string status;   // pending | applied | rejected | reverted | failed
  std::string client;   // MCP client name
  std::string reason;   // justification given by the AI
  std::string summary;  // human readable list of the changes
  std::string changes;  // normalized change items (JSON array)
  std::string before;   // previous values (JSON array, for revert)
  std::string note;     // error text / decision note
  int64_t decidedEpoch = 0;
};

// Bounds for AI changes (tighter than the web interface).
struct AiLimits {
  float comfortMin = 17.f, comfortMax = 24.f, comfortStepK = 1.5f;
  float ecoMin = 14.f, ecoMax = 21.f, ecoStepK = 2.f;
  float frostMin = 5.f, frostMax = 12.f;
  float hystMin = 0.1f, hystMax = 1.5f;
  uint16_t minTimeSec = 60, maxTimeSec = 1800;
  uint16_t maxComfortShiftMin = 180;  // daily comfort time may change by at most 3 h
  float adviceMaxK = 0.5f;
};

class AiState {
 public:
  AiConfig cfg;
  AiLimits limits;
  std::vector<AiChange> journal;  // newest last, max kMaxJournal
  std::string lastClient;          // from the MCP initialize request
  static constexpr size_t kMaxJournal = 40;

  void toJson(JsonObject o, bool includeSecrets) const;
  std::string fromJson(JsonObjectConst o);
  static void changeToJson(const AiChange& c, JsonObject o);
  void journalToJson(JsonArray a, size_t max) const;
  AiChange* find(uint16_t id);
  uint16_t nextId() const;
  void add(AiChange c);
};

// Validates `changes` against the limits and the current configuration.
// On success fills normalized items, previous values and a summary.
std::string aiValidate(HeatCoreSystem& sys, JsonArrayConst changes, JsonArray normalized, JsonArray before,
                       std::string& summary);
// Applies normalized items atomically (all or nothing).
std::string aiApply(HeatCoreSystem& sys, JsonArrayConst normalized);

// Entry points used by MCP and the web interface. `result` receives the
// outcome (status, id, summary, ...). Return value = error text.
std::string aiSubmit(HeatCoreSystem& sys, JsonArrayConst changes, const std::string& reason, bool dryRun,
                     JsonObject result);
std::string aiDecide(HeatCoreSystem& sys, uint16_t id, bool accept, JsonObject result);
std::string aiRevert(HeatCoreSystem& sys, uint16_t id, const std::string& reason, JsonObject result);
// Limits and the reachable parameters (for the AI and the UI).
void aiLimitsToJson(const HeatCoreSystem& sys, JsonObject o);

}  // namespace hc
