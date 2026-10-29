#pragma once
// MQTT client (PubSubClient) running in the network task. Heating control
// never waits for MQTT: outbound messages are queued, inbound commands are
// executed under the system lock.
#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include <atomic>
#include <string>

#include "hc/mqtt/MqttBridge.h"
#include "platform/Queues.h"

namespace platform {

class MqttLink {
 public:
  explicit MqttLink(hc::HeatCoreSystem& sys);
  void loop(bool networkOnline);
  void enqueue(hc::MqttMsg m) { out_.push(std::move(m)); }
  void requestReconnect() { reconnect_ = true; }
  bool connected() const { return up_; }
  // Called from the system event hook (system lock held).
  void onEvent(const hc::Event& e);
  const char* stateText();

 private:
  bool connect();
  void publishAll(bool withDiscovery);
  void onMessage(char* topic, uint8_t* payload, unsigned int len);

  hc::HeatCoreSystem& sys_;
  hc::MqttBridge bridge_;
  NetworkClient net_;
  PubSubClient client_;
  BoundedQueue<hc::MqttMsg> out_{120};
  hc::MqttConfig cfg_;
  std::string host_, clientId_, willTopic_;
  uint32_t lastAttempt_ = 0;
  uint32_t backoffMs_ = 2000;
  uint32_t lastState_ = 0;
  uint32_t lastSignatureCheck_ = 0;
  uint32_t signature_ = 0;
  std::atomic<bool> reconnect_{false};
  bool wasConnected_ = false;
  bool publishing_ = false;
  std::atomic<bool> up_{false};
  std::atomic<bool> enabled_{false};
};

}  // namespace platform
