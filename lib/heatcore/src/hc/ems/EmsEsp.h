#pragma once
// EMS-ESP gateway (BBQKees) for Buderus / Junkers / Nefit EMS heating systems,
// connected over MQTT.
//
// Reading: HeatCore subscribes to the data topics EMS-ESP publishes every few
// seconds in its default "nested" format:
//   <base>/boiler_data      outdoor/flow/return temperature, burner, pressure, hot water ("dhw" object)
//   <base>/thermostat_data  per heating circuit "hc1".."hc4": mode, mode type, reduction mode, setpoints
//   <base>/mixer_data       per heating circuit: flow temperature, mixing valve, pump
//   <base>/status           online / offline
// Texts may be German or English (EMS-ESP language setting), on/off values as
// text ("an"/"aus", "on"/"off"), true/false or 1/0, enums as text or index.
// Outdoor, boiler flow, circuit flow and hot water temperature become the
// sensors "ems:outdoor", "ems:boilerflow", "ems:hcflow", "ems:dhw".
//
// Controlling (heat demand with target "emsesp", one heating circuit):
//   * take over: the reduction mode of the circuit is set to "nofrost"
//     (RC35: "Abschaltbetrieb", Home Assistant: "Frostschutz"), so that
//     "night" really switches the circuit off; the previous reduction mode
//     is remembered (stored, survives restarts),
//   * heat: mode "day", no heat: mode "night",
//   * give back (heat demand switched off, controlled restart): the
//     remembered mode (normally "auto") and reduction mode.
// Supported are thermostats with the modes day / night / auto (RC30, RC35).
// The thermostat data confirm every command: while they are current a
// command is only repeated if the thermostat reports something else.
#include <ArduinoJson.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "hc/core/Clock.h"

namespace hc {

enum class EmsMode : int8_t { Unknown = -1, Night = 0, Day = 1, Auto = 2 };
enum class EmsReduce : int8_t { Unknown = -1, NoFrost = 0, Reduce = 1, Room = 2, Outdoor = 3 };

const char* emsModeKey(EmsMode m);      // "night" | "day" | "auto" | ""
const char* emsReduceKey(EmsReduce r);  // "nofrost" | "reduce" | "room" | "outdoor" | ""
const char* emsModeText(EmsMode m);     // German for the web UI
const char* emsReduceText(EmsReduce r);
EmsMode emsModeParse(JsonVariantConst v);
EmsReduce emsReduceParse(JsonVariantConst v);
EmsReduce emsReduceParse(const char* s);
int emsBoolParse(JsonVariantConst v);  // 1 / 0 / -1 (unknown)

struct EmsEspConfig {
  bool enabled = false;
  std::string base = "ems-esp";  // MQTT base topic of EMS-ESP
  uint8_t hc = 2;                // heating circuit 1..4
  bool sensors = true;           // offer outdoor/flow/hot water as sensors "ems:..."
  // mode and reduction mode before HeatCore took the circuit over
  // ("" = not taken over); runtime values, persisted so that they survive a restart
  std::string savedMode, savedReduce;
};

struct EmsEspState {
  int8_t online = -1;  // <base>/status: 1 online, 0 offline, -1 not seen
  Millis boilerMs = 0, thermoMs = 0, mixerMs = 0;
  uint32_t messages = 0;
  std::string error;  // last parse problem
  // boiler
  float outdoor = NAN, boilerFlow = NAN, boilerReturn = NAN, boilerSetFlow = NAN, pressure = NAN, dhwTemp = NAN,
        burnPower = NAN;
  int8_t heatingActive = -1, burner = -1, dhwActive = -1;
  std::string serviceCode, boilerLastCode;
  // thermostat, configured heating circuit
  EmsMode mode = EmsMode::Unknown;
  std::string modeType;  // "Tag" / "Nacht" / ... as reported
  EmsReduce reduce = EmsReduce::Unknown;
  std::string reduceRaw;  // as reported (when it could not be recognised)
  float selTemp = NAN, targetFlow = NAN, dampedOutdoor = NAN;
  std::string summerMode, heatingType, thermoLastCode;
  bool hcSeen = false;
  // mixer, configured heating circuit
  float hcFlow = NAN, valve = NAN;
  int8_t pump = -1;
};

class EmsEsp {
 public:
  static constexpr Millis kFreshMs = 90000;   // data older than this are not used for decisions
  static constexpr Millis kConfirmMs = 60000;  // time a command gets until the thermostat must report it

  EmsEspConfig cfg;
  EmsEspState st;
  uint32_t revision = 0;

  std::vector<std::string> topics() const;  // empty while disabled
  // Returns true if the topic belongs to EMS-ESP (handled, even if unusable).
  bool ingest(const std::string& topic, const std::string& payload, Millis now);
  bool thermostatFresh(Millis now) const;
  bool boilerFresh(Millis now) const;
  bool mixerFresh(Millis now) const;

  // Command topic for the configured heating circuit, e.g. "ems-esp/thermostat/hc2/mode".
  std::string commandTopic(const char* cmd) const;

  void toJson(JsonObject o) const;  // includes the remembered mode / reduction mode
  // Validated, all or nothing. `keepSaved`: API changes keep the remembered
  // mode / reduction mode, loading from flash restores them.
  std::string fromJson(JsonObjectConst o, bool keepSaved);
  void statusJson(JsonObject o, Millis now) const;
  void clearState() { st = EmsEspState(); }

 private:
  void parseBoiler(JsonObjectConst o, Millis now);
  void parseThermostat(JsonObjectConst o, Millis now);
  void parseMixer(JsonObjectConst o, Millis now);
  std::string hcKey() const { return "hc" + std::to_string(cfg.hc); }
};

}  // namespace hc
