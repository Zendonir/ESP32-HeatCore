#pragma once
// MQTT / Home Assistant bridge (protocol logic only, no network code).
//
// Home Assistant may: set setpoints and modes, trigger rules with a manual
// trigger, select display pages and settings, send RF commands explicitly
// exposed for HA and switch general outputs that are released for HA (always
// time limited). It can never change safety settings, switch heating circuit
// outputs or block local control: every command goes through
// HeatCoreSystem::execute() with source "mqtt".
#include <string>
#include <vector>

#include "hc/system/HeatCoreSystem.h"

namespace hc {

struct MqttMsg {
  std::string topic;
  std::string payload;
  bool retain = false;
};

class MqttBridge {
 public:
  explicit MqttBridge(HeatCoreSystem& sys) : sys_(sys) {}

  std::string base() const { return sys_.baseTopic(); }
  std::string availabilityTopic() const { return base() + "/status"; }
  std::vector<std::string> subscriptions() const;

  // Discovery configs (retained). Entities that disappeared since the last
  // call are removed with an empty retained payload.
  void discovery(std::vector<MqttMsg>& out);
  void state(std::vector<MqttMsg>& out) const;
  void onEvent(const Event& e, std::vector<MqttMsg>& out) const;
  // Returns true if the topic was a known command (handled or rejected).
  bool handle(const std::string& topic, const std::string& payload, std::string* error = nullptr);

  static std::string slug(const std::string& s);

 private:
  void roomState(const Room& r, std::vector<MqttMsg>& out) const;
  void rfDeviceState(const rf::RxDevice& d, std::vector<MqttMsg>& out) const;
  void sensorState(const Sensor& s, std::vector<MqttMsg>& out) const;
  void displayState(std::vector<MqttMsg>& out) const;
  void rfStats(std::vector<MqttMsg>& out) const;
  void alarmState(std::vector<MqttMsg>& out) const;
  std::string uid(const std::string& obj) const;
  void addConfig(std::vector<MqttMsg>& out, std::vector<std::string>& topics, const char* component,
                 const std::string& object, JsonDocument& doc) const;

  HeatCoreSystem& sys_;
  std::vector<std::string> published_;
};

}  // namespace hc
