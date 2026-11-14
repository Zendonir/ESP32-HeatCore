#include "hc/mqtt/MqttBridge.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace hc {

namespace {
const char* const kPageNames[] = {"system", "rooms", "circuits", "diagnostics", "model"};

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> v;
  size_t start = 0;
  while (true) {
    size_t p = s.find(sep, start);
    v.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
    if (p == std::string::npos) break;
    start = p + 1;
  }
  return v;
}

std::string toStr(const JsonDocument& d) {
  std::string s;
  serializeJson(d, s);
  return s;
}

const char* onOff(bool b) { return b ? "ON" : "OFF"; }

const char* haMode(const Room& r) {
  if (r.mode == RoomMode::Off) return "off";
  if (r.mode == RoomMode::Auto) return "auto";
  return "heat";
}
// heating power of a room in %, from the active control state (a room that is
// off or has its window open still heats against its frost protection):
// PWM duty (100 while an external request keeps the valve open), otherwise
// 100 while the room heats; 0 while the heat source delivers no heat
int roomPower(const Room& r, bool supply) {
  if (r.overTemp || !supply) return 0;
  if (r.control == RoomControl::Pwm && r.pwmRunning) {
    bool sustained = r.externalDemand && !r.windowOpen() && r.temperature < r.setpoint + 1.0f;
    return sustained ? 100 : static_cast<int>(std::lround(r.duty));
  }
  return r.demand ? 100 : 0;
}

// effective state as text (in auto mode the schedule decides between comfort and eco)
const char* roomModeText(const Room& r) {
  auto near = [&](float a) { return std::fabs(r.setpoint - a) < 0.05f; };
  return r.mode == RoomMode::Off   ? "Aus"
         : r.windowOpen()         ? "Fenster offen"
         : r.manualActive         ? "Hand"
         : r.preheatFor >= 0      ? "Vorheizen"
         : near(r.comfort)        ? "Komfort"
         : near(r.eco)            ? "Absenkung"
         : near(r.frost)          ? "Frostschutz"
                                  : "Auto";
}

const char* haPreset(const Room& r) {
  switch (r.mode) {
    case RoomMode::Comfort: return "comfort";
    case RoomMode::Eco: return "eco";
    case RoomMode::Frost: return "frost";
    default: return "none";
  }
}
}  // namespace

std::string MqttBridge::slug(const std::string& s) {
  std::string o = s;
  for (auto& c : o)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) c = '_';
  return o;
}

std::string MqttBridge::uid(const std::string& obj) const { return "hc_" + sys_.platform.deviceId + "_" + obj; }

std::vector<std::string> MqttBridge::subscriptions() const {
  std::string b = base();
  std::vector<std::string> s = {b + "/room/+/+/set", b + "/mode/set",   b + "/rf/tx/+/+/send", b + "/display/+/set",
                                b + "/output/+/set", b + "/rule/+/run", b + "/in/#"};
  for (auto& t : sys_.external.mqttTopics()) s.push_back(t);  // other devices (Shelly, Button+, ...)
  for (auto& t : sys_.ems.topics()) s.push_back(t);            // EMS-ESP boiler / thermostat / mixer
  return s;
}

void MqttBridge::addConfig(std::vector<MqttMsg>& out, std::vector<std::string>& topics, const char* component,
                           const std::string& object, JsonDocument& doc) const {
  std::string b = base();
  doc["unique_id"] = uid(object);
  doc["object_id"] = "heatcore_" + object;
  JsonObject dev = doc["device"].to<JsonObject>();
  dev["identifiers"].to<JsonArray>().add("heatcore_" + sys_.platform.deviceId);
  dev["name"] = sys_.sysCfg.deviceName;
  dev["manufacturer"] = "HeatCore";
  dev["model"] = "KinCony KC868-A16 V3";
  dev["sw_version"] = kFirmwareVersion;
  if (doc["availability"].isNull()) doc["availability_topic"] = availabilityTopic();
  std::string topic = sys_.mqttCfg.discoveryPrefix + "/" + component + "/heatcore_" + sys_.platform.deviceId + "/" +
                      object + "/config";
  out.push_back({topic, toStr(doc), true});
  topics.push_back(topic);
}

void MqttBridge::discovery(std::vector<MqttMsg>& out) {
  std::vector<std::string> topics;
  std::string b = base();
  // ---- global mode
  {
    JsonDocument d;
    d["name"] = "Betriebsart";
    d["state_topic"] = b + "/mode/state";
    d["command_topic"] = b + "/mode/set";
    JsonArray o = d["options"].to<JsonArray>();
    for (auto m : {"normal", "eco", "away", "vacation"}) o.add(m);
    addConfig(out, topics, "select", "mode", d);
  }
  // ---- rooms
  for (auto& r : sys_.heating.rooms) {
    std::string st = b + "/room/" + r.id + "/state";
    {
      JsonDocument d;
      d["name"] = r.name;
      d["current_temperature_topic"] = st;
      d["current_temperature_template"] = "{{ value_json.temp }}";
      d["temperature_state_topic"] = st;
      d["temperature_state_template"] = "{{ value_json.setpoint }}";
      d["temperature_command_topic"] = b + "/room/" + r.id + "/setpoint/set";
      d["mode_state_topic"] = st;
      d["mode_state_template"] = "{{ value_json.ha_mode }}";
      d["mode_command_topic"] = b + "/room/" + r.id + "/mode/set";
      JsonArray modes = d["modes"].to<JsonArray>();
      for (auto m : {"auto", "heat", "off"}) modes.add(m);
      d["preset_mode_state_topic"] = st;
      d["preset_mode_value_template"] = "{{ value_json.preset }}";
      d["preset_mode_command_topic"] = b + "/room/" + r.id + "/preset/set";
      JsonArray presets = d["preset_modes"].to<JsonArray>();
      for (auto p : {"comfort", "eco", "frost"}) presets.add(p);
      d["action_topic"] = st;
      d["action_template"] = "{{ value_json.action }}";
      d["min_temp"] = sys_.safety.cfg.minSetpoint;
      d["max_temp"] = sys_.safety.cfg.maxSetpoint;
      d["temp_step"] = 0.5;
      d["precision"] = 0.1;
      d["temperature_unit"] = "C";
      d["json_attributes_topic"] = st;
      d["json_attributes_template"] =
          "{{ {'heizleistung': value_json.power, 'betrieb': value_json.mode_text, 'grund': value_json.reason, "
          "'heizbedarf': value_json.demand} | tojson }}";
      addConfig(out, topics, "climate", "room_" + r.id, d);
    }
    {
      JsonDocument d;
      d["name"] = r.name + " Fenster";
      d["state_topic"] = st;
      d["value_template"] = "{{ 'ON' if value_json.window else 'OFF' }}";
      d["device_class"] = "window";
      addConfig(out, topics, "binary_sensor", "room_" + r.id + "_window", d);
    }
    {
      JsonDocument d;
      d["name"] = r.name + " Heizbedarf";
      d["state_topic"] = st;
      d["value_template"] = "{{ 'ON' if value_json.demand else 'OFF' }}";
      d["device_class"] = "heat";
      addConfig(out, topics, "binary_sensor", "room_" + r.id + "_demand", d);
    }
    {
      JsonDocument d;
      d["name"] = r.name + " Heizleistung";
      d["state_topic"] = st;
      d["value_template"] = "{{ value_json.power }}";
      d["unit_of_measurement"] = "%";
      d["state_class"] = "measurement";
      d["icon"] = "mdi:radiator";
      addConfig(out, topics, "sensor", "room_" + r.id + "_power", d);
    }
    {
      JsonDocument d;
      d["name"] = r.name + " Solltemperatur";
      d["state_topic"] = st;
      d["value_template"] = "{{ value_json.setpoint }}";
      d["device_class"] = "temperature";
      d["unit_of_measurement"] = "\xC2\xB0" "C";
      d["state_class"] = "measurement";
      addConfig(out, topics, "sensor", "room_" + r.id + "_setpoint", d);
    }
    {
      JsonDocument d;
      d["name"] = r.name + " Betrieb";
      d["state_topic"] = st;
      d["value_template"] = "{{ value_json.mode_text }}";
      d["json_attributes_topic"] = st;
      d["json_attributes_template"] = "{{ {'grund': value_json.reason, 'hand': value_json.manual} | tojson }}";
      d["icon"] = "mdi:information-outline";
      addConfig(out, topics, "sensor", "room_" + r.id + "_status", d);
    }
    for (auto which : {"comfort", "eco"}) {
      bool comfort = std::strcmp(which, "comfort") == 0;
      JsonDocument d;
      d["name"] = r.name + (comfort ? " Komforttemperatur" : " Absenktemperatur");
      d["state_topic"] = st;
      d["value_template"] = std::string("{{ value_json.") + which + " }}";
      d["command_topic"] = b + "/room/" + r.id + "/" + which + "/set";
      d["min"] = sys_.safety.cfg.minSetpoint;
      d["max"] = sys_.safety.cfg.maxSetpoint;
      d["step"] = 0.5;
      d["mode"] = "box";
      d["device_class"] = "temperature";
      d["unit_of_measurement"] = "\xC2\xB0" "C";
      d["entity_category"] = "config";
      addConfig(out, topics, "number", "room_" + r.id + "_" + which, d);
    }
    {
      JsonDocument d;
      d["name"] = r.name + " Lernmodell";
      d["state_topic"] = b + "/model/" + r.id + "/state";
      d["value_template"] = "{{ {'ready': 'bereit', 'learning': 'lernt', 'degraded': 'eingeschr\xC3\xA4nkt', 'disabled': 'aus'}.get(value_json.status, value_json.status) }}";
      d["json_attributes_topic"] = b + "/model/" + r.id + "/state";
      d["entity_category"] = "diagnostic";
      addConfig(out, topics, "sensor", "model_" + r.id, d);
    }
  }
  // ---- RF receive devices
  for (auto& dev : sys_.rfReg.rx) {
    std::string id = std::to_string(dev.id);
    bool hasState = false, hasButtons = false;
    for (auto& btn : dev.buttons) {
      if (btn.role == rf::ButtonRole::Button) hasButtons = true;
      else hasState = true;
    }
    auto avail = [&](JsonDocument& d) {
      JsonArray a = d["availability"].to<JsonArray>();
      a.add<JsonObject>()["topic"] = availabilityTopic();
      a.add<JsonObject>()["topic"] = b + "/rf/" + id + "/avail";
      d["availability_mode"] = "all";
    };
    if (hasState) {
      JsonDocument d;
      d["name"] = dev.name;
      d["state_topic"] = b + "/rf/" + id + "/state";
      switch (dev.function) {
        case rf::InputFunction::Window: d["device_class"] = "window"; break;
        case rf::InputFunction::Presence: d["device_class"] = "occupancy"; break;
        case rf::InputFunction::HeatDemand: d["device_class"] = "heat"; break;
        default: break;
      }
      avail(d);
      addConfig(out, topics, "binary_sensor", "rf_" + id, d);
    }
    if (hasButtons) {
      JsonDocument d;
      d["name"] = dev.name;
      d["state_topic"] = b + "/rf/" + id + "/event";
      JsonArray t = d["event_types"].to<JsonArray>();
      for (auto g : {"press", "short", "long", "release"}) t.add(g);
      avail(d);
      addConfig(out, topics, "event", "rf_" + id + "_event", d);
    }
    JsonDocument d;
    d["name"] = dev.name + " Empfangsqualit\xC3\xA4t";
    d["state_topic"] = b + "/rf/" + id + "/diag";
    d["value_template"] = "{{ value_json.reliability }}";
    d["unit_of_measurement"] = "%";
    d["json_attributes_topic"] = b + "/rf/" + id + "/diag";
    d["entity_category"] = "diagnostic";
    addConfig(out, topics, "sensor", "rf_" + id + "_quality", d);
  }
  // ---- sensors (1-Wire, RF433, Bluetooth, external)
  for (auto& s : sys_.sensors.sensors()) {
    std::string sl = slug(s.id);
    std::string st = b + "/sensor/" + sl + "/state";
    auto avail = [&](JsonDocument& d) {
      JsonArray a = d["availability"].to<JsonArray>();
      a.add<JsonObject>()["topic"] = availabilityTopic();
      a.add<JsonObject>()["topic"] = b + "/sensor/" + sl + "/avail";
      d["availability_mode"] = "all";
    };
    {
      JsonDocument d;
      d["name"] = s.name;
      d["state_topic"] = st;
      d["value_template"] = "{{ value_json.t }}";
      d["device_class"] = "temperature";
      d["unit_of_measurement"] = "\xC2\xB0" "C";
      d["state_class"] = "measurement";
      avail(d);
      addConfig(out, topics, "sensor", "sensor_" + sl, d);
    }
    if (s.hasHumidity) {
      JsonDocument d;
      d["name"] = s.name + " Feuchte";
      d["state_topic"] = st;
      d["value_template"] = "{{ value_json.h }}";
      d["device_class"] = "humidity";
      d["unit_of_measurement"] = "%";
      avail(d);
      addConfig(out, topics, "sensor", "sensor_" + sl + "_h", d);
    }
    if (s.batteryOk >= 0) {
      JsonDocument d;
      d["name"] = s.name + " Batterie";
      d["state_topic"] = st;
      d["value_template"] = "{{ 'ON' if value_json.bat_low else 'OFF' }}";
      d["device_class"] = "battery";
      d["entity_category"] = "diagnostic";
      addConfig(out, topics, "binary_sensor", "sensor_" + sl + "_bat", d);
    }
    if (s.batteryPct >= 0) {
      JsonDocument d;
      d["name"] = s.name + " Batteriestand";
      d["state_topic"] = st;
      d["value_template"] = "{{ value_json.bat }}";
      d["device_class"] = "battery";
      d["unit_of_measurement"] = "%";
      d["state_class"] = "measurement";
      d["entity_category"] = "diagnostic";
      addConfig(out, topics, "sensor", "sensor_" + sl + "_batpct", d);
    }
  }
  // ---- RF statistics and transmitter
  struct StatDef {
    const char* key;
    const char* name;
    const char* unit;
  };
  for (auto& sd : {StatDef{"frames", "Funk Telegramme", ""}, StatDef{"unknown", "Funk unbekannte Codes", ""},
                   StatDef{"rejected", "Funk verworfen", ""}, StatDef{"quality", "Funk Signalqualit\xC3\xA4t", "%"},
                   StatDef{"noise", "Funk St\xC3\xB6rpegel", "Flanken/s"}}) {
    JsonDocument d;
    d["name"] = sd.name;
    d["state_topic"] = b + "/rf/stats";
    d["value_template"] = std::string("{{ value_json.") + sd.key + " }}";
    if (*sd.unit) d["unit_of_measurement"] = sd.unit;
    d["entity_category"] = "diagnostic";
    addConfig(out, topics, "sensor", std::string("rf_stat_") + sd.key, d);
  }
  {
    JsonDocument d;
    d["name"] = "Funksender Status";
    d["state_topic"] = b + "/rf/tx/state";
    d["value_template"] = "{{ {'idle': 'bereit', 'busy': 'sendet', 'disabled': 'aus', 'no_hw': 'keine Hardware'}.get(value_json.status, value_json.status) }}";
    d["json_attributes_topic"] = b + "/rf/tx/state";
    d["entity_category"] = "diagnostic";
    addConfig(out, topics, "sensor", "rf_tx", d);
  }
  for (auto& dev : sys_.rfReg.tx) {
    for (auto& c : dev.commands) {
      if (!c.haExposed || !dev.enabled) continue;
      JsonDocument d;
      d["name"] = dev.name + " " + c.name;
      d["command_topic"] = b + "/rf/tx/" + std::to_string(dev.id) + "/" + std::to_string(c.id) + "/send";
      d["payload_press"] = "PRESS";
      addConfig(out, topics, "button", "rf_tx_" + std::to_string(dev.id) + "_" + std::to_string(c.id), d);
    }
  }
  // ---- display
  {
    JsonDocument d;
    d["name"] = "Display";
    d["state_topic"] = b + "/display/state";
    d["value_template"] = "{{ value_json.enabled }}";
    d["command_topic"] = b + "/display/enabled/set";
    addConfig(out, topics, "switch", "display", d);
  }
  {
    JsonDocument d;
    d["name"] = "Display Kontrast";
    d["state_topic"] = b + "/display/state";
    d["value_template"] = "{{ value_json.contrast }}";
    d["command_topic"] = b + "/display/contrast/set";
    d["min"] = 0;
    d["max"] = 255;
    d["entity_category"] = "config";
    addConfig(out, topics, "number", "display_contrast", d);
  }
  {
    JsonDocument d;
    d["name"] = "Display Seite";
    d["state_topic"] = b + "/display/state";
    d["value_template"] = "{{ value_json.page }}";
    d["command_topic"] = b + "/display/page/set";
    JsonArray o = d["options"].to<JsonArray>();
    for (auto p : kPageNames) o.add(p);
    addConfig(out, topics, "select", "display_page", d);
  }
  {
    JsonDocument d;
    d["name"] = "Display St\xC3\xB6rung";
    d["state_topic"] = b + "/display/state";
    d["value_template"] = "{{ value_json.problem }}";
    d["device_class"] = "problem";
    d["entity_category"] = "diagnostic";
    addConfig(out, topics, "binary_sensor", "display_problem", d);
  }
  // ---- outputs released for HA, heating circuits
  for (int i = 0; i < kNumOutputs; ++i) {
    const OutputConfig& oc = sys_.io.outCfg[static_cast<size_t>(i)];
    std::string n = std::to_string(i + 1);
    if (oc.function == OutputFunction::General && oc.haAllowed) {
      JsonDocument d;
      d["name"] = oc.name;
      d["state_topic"] = b + "/output/" + n + "/state";
      d["command_topic"] = b + "/output/" + n + "/set";
      addConfig(out, topics, "switch", "output_" + n, d);
    }
    const Circuit& c = sys_.heating.circuits[static_cast<size_t>(i)];
    if (oc.function == OutputFunction::Circuit && !c.room.empty()) {
      JsonDocument d;
      d["name"] = c.name + " Ventil";
      d["state_topic"] = b + "/circuit/" + n + "/state";
      d["value_template"] = "{{ value_json.open }}";
      d["device_class"] = "opening";
      d["json_attributes_topic"] = b + "/circuit/" + n + "/state";
      d["json_attributes_template"] =
          "{{ {'raum': value_json.room, 'hand': value_json.manual, 'anteil': value_json.share} | tojson }}";
      addConfig(out, topics, "binary_sensor", "circuit_" + n, d);
      JsonDocument f;
      f["name"] = c.name + " St\xC3\xB6rung";
      f["state_topic"] = b + "/circuit/" + n + "/state";
      f["value_template"] = "{{ value_json.fault }}";
      f["device_class"] = "problem";
      addConfig(out, topics, "binary_sensor", "circuit_" + n + "_fault", f);
    }
  }
  // ---- system: total heating power, rooms heating, network and device health
  {
    std::string st = b + "/system/state";
    struct SysDef {
      const char* key;
      const char* name;
      const char* unit;
      const char* deviceClass;
      const char* icon;
      bool diagnostic;
      bool measurement;
    };
    for (auto& sd : {SysDef{"power", "Heizleistung gesamt", "%", "", "mdi:radiator", false, true},
                     SysDef{"rooms_heating", "R\xC3\xA4ume heizen", "", "", "mdi:home-thermometer", false, true},
                     SysDef{"uptime_h", "Laufzeit", "h", "duration", "", true, false},
                     SysDef{"rssi", "WLAN Signal", "dBm", "signal_strength", "", true, true},
                     SysDef{"heap_kb", "Freier Speicher", "kB", "", "mdi:memory", true, true},
                     SysDef{"network", "Netzwerk", "", "", "mdi:lan", true, false},
                     SysDef{"ip", "IP-Adresse", "", "", "mdi:ip-network", true, false}}) {
      JsonDocument d;
      d["name"] = sd.name;
      d["state_topic"] = st;
      d["value_template"] = std::string("{{ value_json.") + sd.key + " }}";
      if (*sd.unit) d["unit_of_measurement"] = sd.unit;
      if (*sd.deviceClass) d["device_class"] = sd.deviceClass;
      if (*sd.icon) d["icon"] = sd.icon;
      if (sd.measurement) d["state_class"] = "measurement";
      if (sd.diagnostic) d["entity_category"] = "diagnostic";
      addConfig(out, topics, "sensor", std::string("sys_") + sd.key, d);
    }
  }
  // ---- alarms, rules
  {
    JsonDocument d;
    d["name"] = "St\xC3\xB6rung";
    d["state_topic"] = b + "/alarm/state";
    d["value_template"] = "{{ value_json.problem }}";
    d["json_attributes_topic"] = b + "/alarm/state";
    d["device_class"] = "problem";
    addConfig(out, topics, "binary_sensor", "alarm", d);
  }
  for (auto& r : sys_.automation.rules()) {
    bool manual = false;
    for (auto& t : r.triggers) manual |= t.type == TriggerType::Manual;
    if (!manual || !r.enabled) continue;
    JsonDocument d;
    d["name"] = "Regel " + r.name;
    d["command_topic"] = b + "/rule/" + std::to_string(r.id) + "/run";
    d["payload_press"] = "PRESS";
    addConfig(out, topics, "button", "rule_" + std::to_string(r.id), d);
  }
  // ---- 1-Wire sensors had the bus in their id ("ow1:..."): remove those
  // entities once, they are replaced by the bus-independent ones ("ow:...")
  if (!legacyCleared_) {
    legacyCleared_ = true;
    for (auto& s : sys_.sensors.sensors()) {
      if (s.kind != SensorKind::OneWire || s.id.rfind("ow:", 0) != 0) continue;
      for (int bus = 1; bus <= 3; ++bus) {
        std::string object = "sensor_ow" + std::to_string(bus) + "_" + s.id.substr(3);
        out.push_back({sys_.mqttCfg.discoveryPrefix + "/sensor/heatcore_" + sys_.platform.deviceId + "/" + object +
                           "/config",
                       "", true});
      }
    }
  }
  // ---- heat demand (boiler / heating circuit)
  if (sys_.heatDemand.cfg.supply != "off") {
    JsonDocument d;
    d["name"] = "W\xC3\xA4rmeangebot";
    d["state_topic"] = b + "/system/state";
    d["value_template"] = "{{ value_json.supply }}";
    d["json_attributes_topic"] = b + "/system/state";
    d["json_attributes_template"] = "{{ {'grund': value_json.supply_text} | tojson }}";
    d["device_class"] = "heat";
    addConfig(out, topics, "binary_sensor", "heatsupply", d);
  }
  if (sys_.heatDemand.cfg.enabled) {
    JsonDocument d;
    d["name"] = "W\xC3\xA4rmeanforderung";
    d["state_topic"] = b + "/heatdemand/state";
    d["device_class"] = "heat";
    addConfig(out, topics, "binary_sensor", "heatdemand", d);
  }
  // ---- remove entities that no longer exist
  for (auto& old : published_)
    if (std::find(topics.begin(), topics.end(), old) == topics.end()) out.push_back({old, "", true});
  published_ = std::move(topics);
}

// ------------------------------------------------------------------ state

void MqttBridge::roomState(const Room& r, std::vector<MqttMsg>& out) const {
  JsonDocument d;
  if (r.hasTemp) d["temp"] = std::round(r.temperature * 10.f) / 10.f;
  else d["temp"] = nullptr;
  d["setpoint"] = r.setpoint;
  d["ha_mode"] = haMode(r);
  d["preset"] = haPreset(r);
  d["action"] = r.mode == RoomMode::Off ? "off" : (r.demand ? "heating" : "idle");
  d["window"] = r.windowOpen();
  d["demand"] = r.demand;
  d["reason"] = r.reason;
  d["manual"] = r.manualActive;
  if (r.control == RoomControl::Pwm && r.pwmRunning) d["duty"] = std::round(r.duty);
  d["power"] = roomPower(r, sys_.heating.supplyOk);
  d["mode_text"] = roomModeText(r);
  d["comfort"] = r.comfort;
  d["eco"] = r.eco;
  out.push_back({base() + "/room/" + r.id + "/state", toStr(d), true});
  if (!sys_.mqttCfg.roomValueTopics) return;
  // one plain value per topic for simple displays (Button+, ...)
  std::string p = base() + "/room/" + r.id + "/";
  auto num = [](float v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%.1f", static_cast<double>(v));
    return std::string(b);
  };
  auto de = [&](float v) {
    std::string s = num(v);
    std::replace(s.begin(), s.end(), '.', ',');
    return s;
  };
  out.push_back({p + "temp", r.hasTemp ? num(r.temperature) : std::string(), true});
  out.push_back({p + "setpoint", num(r.setpoint), true});
  out.push_back({p + "heating", r.demand ? "1" : "0", true});
  out.push_back({p + "window", r.windowOpen() ? "1" : "0", true});
  out.push_back({p + "mode_text", roomModeText(r), true});
  const Sensor* sen = r.tempSensor.empty() ? nullptr : sys_.sensors.find(r.tempSensor);
  if (sen && sen->hasHumidity) out.push_back({p + "humidity", num(sen->humidity), true});
  std::string text = (r.hasTemp ? de(r.temperature) : std::string("--")) + "° / " + de(r.setpoint) + "°" +
                     (r.windowOpen() ? " Fenster" : r.demand ? " heizt" : "");
  out.push_back({p + "text", text, true});
}

void MqttBridge::rfDeviceState(const rf::RxDevice& dev, std::vector<MqttMsg>& out) const {
  std::string p = base() + "/rf/" + std::to_string(dev.id);
  if (dev.state >= 0) out.push_back({p + "/state", onOff(dev.state == 1), true});
  out.push_back({p + "/avail", dev.available && dev.enabled ? "online" : "offline", true});
  JsonDocument d;
  d["reliability"] = dev.reliability();
  d["quality"] = static_cast<int>(dev.stats.qualityAvg + 0.5f);
  d["frames"] = dev.stats.frames;
  d["presses"] = dev.stats.presses;
  d["rejected"] = dev.stats.rejected;
  d["last_seen"] = dev.stats.lastEpoch;
  out.push_back({p + "/diag", toStr(d), true});
}

void MqttBridge::sensorState(const Sensor& s, std::vector<MqttMsg>& out) const {
  std::string p = base() + "/sensor/" + slug(s.id);
  if (s.status != SensorStatus::Never) {
    JsonDocument d;
    d["t"] = std::round(s.value * 100.f) / 100.f;
    if (s.hasHumidity) d["h"] = s.humidity;
    if (s.batteryOk >= 0) d["bat_low"] = s.batteryOk == 0;
    if (s.batteryPct >= 0) d["bat"] = s.batteryPct;
    d["status"] = sensorStatusName(s.status);
    out.push_back({p + "/state", toStr(d), true});
  }
  out.push_back({p + "/avail", s.available() ? "online" : "offline", true});
}

void MqttBridge::displayState(std::vector<MqttMsg>& out) const {
  JsonDocument d;
  d["enabled"] = onOff(sys_.display.cfg.enabled);
  d["contrast"] = sys_.display.cfg.contrast;
  d["page"] = kPageNames[sys_.display.page()];
  d["screen"] = onOff(sys_.display.screenOn());
  d["problem"] = onOff(sys_.display.cfg.enabled && !sys_.display.hw.initialized);
  out.push_back({base() + "/display/state", toStr(d), true});
}

void MqttBridge::rfStats(std::vector<MqttMsg>& out) const {
  const auto& s = sys_.rfRx.stats();
  JsonDocument d;
  d["frames"] = s.frames;
  d["unknown"] = s.unknown;
  d["rejected"] = s.rejected;
  d["quality"] = static_cast<int>(s.qualityAvg + 0.5f);
  d["noise"] = s.edgesPerSec;
  out.push_back({base() + "/rf/stats", toStr(d), true});
  JsonDocument t;
  const auto& ts = sys_.rfTx.stats();
  t["status"] = !sys_.rfTx.config().enabled ? "disabled"
                                            : (sys_.rfTx.busy() ? "busy" : (sys_.platform.rfTxHw ? "idle" : "no_hw"));
  uint32_t budget = sys_.rfTx.budgetMs();
  t["airtime_pct_of_budget"] = budget ? 100.f * sys_.rfTx.airtimeLastHourMs() / budget : 0.f;
  t["queue"] = static_cast<int>(sys_.rfTx.queueLength());
  t["sent"] = ts.sent;
  t["failed"] = ts.failed;
  t["last_error"] = ts.lastError;
  out.push_back({base() + "/rf/tx/state", toStr(t), true});
}

void MqttBridge::circuitState(int i, std::vector<MqttMsg>& out) const {
  const Circuit& c = sys_.heating.circuits[static_cast<size_t>(i)];
  JsonDocument d;
  d["open"] = onOff(sys_.io.out[static_cast<size_t>(i)].level != sys_.heating.circuitSafeLevel(c));
  d["fault"] = onOff(c.fault);
  const Room* r = sys_.heating.room(c.room);
  d["room"] = r ? r->name : c.room;
  d["manual"] = c.manual == CircuitManual::Percent
                    ? std::to_string(c.manualPct) + " %"
                    : std::string(c.manual == CircuitManual::Auto ? "" : circuitManualName(c.manual));
  d["share"] = c.share;
  out.push_back({base() + "/circuit/" + std::to_string(i + 1) + "/state", toStr(d), true});
}

void MqttBridge::systemState(std::vector<MqttMsg>& out) const {
  const PlatformStatus& p = sys_.platform;
  JsonDocument d;
  int rooms = 0, heating = 0, power = 0;
  for (auto& r : sys_.heating.rooms) {
    int pw = roomPower(r, sys_.heating.supplyOk);
    if (r.mode == RoomMode::Off && pw == 0) continue;  // switched off and not frost-heating
    ++rooms;
    power += pw;
    if (pw > 0) ++heating;
  }
  d["power"] = rooms ? static_cast<int>(std::lround(static_cast<double>(power) / rooms)) : 0;
  d["rooms_heating"] = heating;
  d["uptime_h"] = std::round(p.uptimeSec / 360.0) / 10.0;
  if (p.wifiConnected && p.network == "WLAN") d["rssi"] = p.wifiRssi;
  else d["rssi"] = nullptr;
  d["heap_kb"] = p.freeHeap / 1024u;
  d["network"] = p.network.empty() ? "-" : p.network;
  d["ip"] = p.ip.empty() ? "-" : p.ip;
  if (sys_.heatDemand.cfg.supply != "off") {
    d["supply"] = onOff(sys_.heating.supplyOk);
    d["supply_text"] = sys_.heatDemand.supplyText;
  }
  out.push_back({base() + "/system/state", toStr(d), true});
}

void MqttBridge::alarmState(std::vector<MqttMsg>& out) const {
  JsonDocument d;
  d["count"] = static_cast<int>(sys_.alarms.count());
  d["problem"] = onOff(sys_.alarms.count() > 0);
  if (!sys_.alarms.list().empty()) d["top"] = sys_.alarms.list().back().message;
  out.push_back({base() + "/alarm/state", toStr(d), true});
}

void MqttBridge::state(std::vector<MqttMsg>& out) const {
  std::string b = base();
  out.push_back({availabilityTopic(), "online", true});
  out.push_back({b + "/mode/state", globalModeName(sys_.heating.globalMode), true});
  if (sys_.heatDemand.cfg.enabled) out.push_back({b + "/heatdemand/state", sys_.heatDemand.active ? "ON" : "OFF", true});
  for (auto& r : sys_.heating.rooms) {
    roomState(r, out);
    if (const ThermalModel* m = sys_.model.find(r.id)) {
      JsonDocument d;
      d["status"] = modelStatusName(m->status());
      d["gain_kh"] = m->heatingGain();
      d["trend_kh"] = m->trendKh();
      d["delay_min"] = m->delayMin();
      d["fault_suspected"] = m->faultSuspected();
      out.push_back({b + "/model/" + r.id + "/state", toStr(d), true});
    }
  }
  for (auto& dev : sys_.rfReg.rx) rfDeviceState(dev, out);
  for (auto& s : sys_.sensors.sensors()) sensorState(s, out);
  rfStats(out);
  displayState(out);
  alarmState(out);
  systemState(out);
  for (int i = 0; i < kNumOutputs; ++i) {
    std::string n = std::to_string(i + 1);
    const OutputConfig& oc = sys_.io.outCfg[static_cast<size_t>(i)];
    if (oc.function == OutputFunction::General && oc.haAllowed)
      out.push_back({b + "/output/" + n + "/state", onOff(sys_.io.out[static_cast<size_t>(i)].level), true});
    const Circuit& c = sys_.heating.circuits[static_cast<size_t>(i)];
    if (oc.function == OutputFunction::Circuit && !c.room.empty()) {
      circuitState(i, out);
    }
  }
}

void MqttBridge::onEvent(const Event& e, std::vector<MqttMsg>& out) const {
  std::string b = base();
  switch (e.type) {
    case EventType::InputGesture:
      if (e.source == EventSource::Rf) {
        const rf::RxDevice* dev = sys_.rfReg.rxDevice(static_cast<uint16_t>(e.a));
        if (!dev) break;
        if (e.gesture == Gesture::Press || e.gesture == Gesture::Short || e.gesture == Gesture::Long ||
            e.gesture == Gesture::Release) {
          JsonDocument d;
          d["event_type"] = gestureName(e.gesture);
          const rf::RxButton* btn = e.b >= 0 ? dev->button(static_cast<uint8_t>(e.b)) : nullptr;
          d["button"] = btn ? btn->name : "";
          d["button_index"] = e.b;
          out.push_back({b + "/rf/" + std::to_string(dev->id) + "/event", toStr(d), false});
        } else {
          rfDeviceState(*dev, out);
        }
      }
      break;
    case EventType::RoomSetpointChanged:
    case EventType::RoomModeChanged:
    case EventType::RoomHeatingChanged:
    case EventType::RoomWindowChanged:
    case EventType::RoomPresenceChanged:
      if (const Room* r = sys_.heating.room(e.ref)) roomState(*r, out);
      break;
    case EventType::GlobalModeChanged: out.push_back({b + "/mode/state", e.ref, true}); break;
    case EventType::SensorValue:
    case EventType::SensorFault:
    case EventType::SensorRecovered:
      if (const Sensor* s = sys_.sensors.find(e.ref)) sensorState(*s, out);
      break;
    case EventType::OutputChanged: {
      int i = e.a - 1;
      if (i < 0 || i >= kNumOutputs) break;
      const OutputConfig& oc = sys_.io.outCfg[static_cast<size_t>(i)];
      std::string n = std::to_string(e.a);
      if (oc.function == OutputFunction::General && oc.haAllowed)
        out.push_back({b + "/output/" + n + "/state", onOff(e.value != 0.f), true});
      const Circuit& c = sys_.heating.circuits[static_cast<size_t>(i)];
      if (oc.function == OutputFunction::Circuit && !c.room.empty()) {
        circuitState(i, out);
      }
      break;
    }
    case EventType::Alarm:
    case EventType::AlarmCleared: alarmState(out); break;
    case EventType::DisplayPageChanged: displayState(out); break;
    case EventType::RfSent:
    case EventType::RfSendFailed:
    case EventType::RfUnknownCode: rfStats(out); break;
    case EventType::Notification: {
      JsonDocument d;
      d["message"] = e.text;
      d["level"] = static_cast<int>(e.value);
      out.push_back({b + "/notify", toStr(d), false});
      break;
    }
    default: break;
  }
}

// --------------------------------------------------------------- commands

bool MqttBridge::handle(const std::string& topic, const std::string& payload, std::string* error) {
  // values and buttons of other devices (external sources)
  // EMS-ESP data; an external source may use the same topic (e.g. another value)
  bool ems = sys_.emsIngest(topic, payload);
  bool external = sys_.external.ingestMqtt(sys_, topic, payload, sys_.clock().nowMs()) || ems;
  std::string b = base() + "/";
  if (topic.compare(0, b.size(), b) != 0) return external;
  std::vector<std::string> p = split(topic.substr(b.size()), '/');
  ActionOrigin origin{EventSource::Mqtt, 0, 0, false};
  std::string err;
  bool known = true;
  if (p.size() == 4 && p[0] == "room" && (p[2] == "comfort" || p[2] == "eco") && p[3] == "set") {
    // stored comfort / eco temperature (number entities in Home Assistant)
    Room* r = sys_.heating.room(p[1]);
    char* end = nullptr;
    float v = std::strtof(payload.c_str(), &end);
    if (!r) err = "Raum nicht gefunden";
    else if (!end || end == payload.c_str() || !std::isfinite(v)) err = "Zahl erwartet";
    else
      err = sys_.heating.setPresetTemp(*r, p[2], v, sys_.safety.cfg.minSetpoint, sys_.safety.cfg.maxSetpoint,
                                       sys_.clock().nowMs(), EventSource::Mqtt);
    if (r && err.empty()) sys_.markDirty(ConfigSection::Heating);
  } else if (p.size() == 4 && p[0] == "room" && p[3] == "set") {
    Action a;
    a.room = p[1];
    if (p[2] == "setpoint") {
      a.type = ActionType::SetSetpoint;
      char* end = nullptr;
      float v = std::strtof(payload.c_str(), &end);
      if (!end || end == payload.c_str() || !std::isfinite(v)) err = "Zahl erwartet";
      a.value = v < sys_.safety.cfg.minSetpoint ? sys_.safety.cfg.minSetpoint
                                                : (v > sys_.safety.cfg.maxSetpoint ? sys_.safety.cfg.maxSetpoint : v);
    } else if (p[2] == "mode") {
      a.type = ActionType::SetRoomMode;
      a.text = payload == "auto" ? "auto" : payload == "heat" ? "comfort" : payload == "off" ? "off" : "";
    } else if (p[2] == "preset") {
      a.type = ActionType::SetRoomMode;
      a.text = payload == "none" ? "auto" : payload;
    } else {
      known = false;
    }
    if (known && err.empty()) err = sys_.execute(a, origin);
  } else if (p.size() == 2 && p[0] == "mode" && p[1] == "set") {
    Action a;
    a.type = ActionType::SetGlobalMode;
    a.text = payload;
    err = sys_.execute(a, origin);
  } else if (p.size() == 5 && p[0] == "rf" && p[1] == "tx" && p[4] == "send") {
    Action a;
    a.type = ActionType::RfSend;
    a.target = p[2] + "/" + p[3];
    err = sys_.execute(a, origin);
  } else if (p.size() == 3 && p[0] == "display" && p[2] == "set") {
    if (p[1] == "enabled") {
      sys_.display.cfg.enabled = payload == "ON" || payload == "1" || payload == "true";
      sys_.markDirty(ConfigSection::Display);
    } else if (p[1] == "contrast") {
      int v = std::atoi(payload.c_str());
      if (v < 0 || v > 255) err = "Kontrast 0..255";
      else {
        sys_.display.cfg.contrast = static_cast<uint8_t>(v);
        sys_.markDirty(ConfigSection::Display);
      }
    } else if (p[1] == "page") {
      int page = std::atoi(payload.c_str());
      for (int i = 0; i < kNumPages; ++i)
        if (payload == kPageNames[i]) page = i + 1;
      Action a;
      a.type = ActionType::DisplayPage;
      a.index = page;
      err = sys_.execute(a, origin);
    } else {
      known = false;
    }
  } else if (p.size() == 3 && p[0] == "output" && p[2] == "set") {
    bool on = payload == "ON" || payload == "1" || payload == "true";
    err = sys_.requestOutput(std::atoi(p[1].c_str()), on, 0, EventSource::Mqtt);
  } else if (p.size() == 3 && p[0] == "rule" && p[2] == "run") {
    err = sys_.runRule(static_cast<uint16_t>(std::atoi(p[1].c_str())), EventSource::Mqtt);
  } else if (p.size() >= 2 && p[0] == "in") {
    std::string suffix = topic.substr(b.size() + 3);
    if (suffix.size() > 23) err = "Thema zu lang";
    else sys_.mqttInbound(suffix, payload.substr(0, 70));
  } else {
    known = false;
  }
  if (error) *error = err;
  return known;
}

}  // namespace hc
