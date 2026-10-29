#include "platform/MqttLink.h"

#include <mutex>

#include "hc/core/Log.h"

namespace platform {

namespace {
constexpr const char* TAG = "mqtt";
}

MqttLink::MqttLink(hc::HeatCoreSystem& sys) : sys_(sys), bridge_(sys), client_(net_) {
  client_.setBufferSize(4096);
  client_.setKeepAlive(30);
  client_.setSocketTimeout(5);
  client_.setCallback([this](char* t, uint8_t* p, unsigned int l) { onMessage(t, p, l); });
}

const char* MqttLink::stateText() {
  if (!enabled_) return "aus";
  return up_ ? "verbunden" : "getrennt";
}

void MqttLink::onEvent(const hc::Event& e) {
  if (!enabled_ || !up_) return;  // called from the control task: no socket access here
  std::vector<hc::MqttMsg> msgs;
  bridge_.onEvent(e, msgs);
  for (auto& m : msgs) out_.push(std::move(m));
}

void MqttLink::onMessage(char* topic, uint8_t* payload, unsigned int len) {
  std::string t(topic);
  std::string p(reinterpret_cast<const char*>(payload), len);
  if (t == cfg_.discoveryPrefix + "/status") {
    if (p == "online") publishAll(true);  // Home Assistant restarted
    return;
  }
  std::string err;
  {
    std::lock_guard<std::recursive_mutex> lk(sys_.mutex());
    bridge_.handle(t, p, &err);
  }
  if (!err.empty()) HC_LOGW(TAG, "command %s rejected: %s", t.c_str(), err.c_str());
}

bool MqttLink::connect() {
  {
    std::lock_guard<std::recursive_mutex> lk(sys_.mutex());
    cfg_ = sys_.mqttCfg;
    willTopic_ = bridge_.availabilityTopic();
    clientId_ = "heatcore-" + sys_.platform.deviceId;
  }
  host_ = cfg_.host;
  client_.setServer(host_.c_str(), cfg_.port);
  bool ok = cfg_.user.empty()
                ? client_.connect(clientId_.c_str(), nullptr, nullptr, willTopic_.c_str(), 1, true, "offline")
                : client_.connect(clientId_.c_str(), cfg_.user.c_str(), cfg_.password.c_str(), willTopic_.c_str(), 1,
                                  true, "offline");
  if (!ok) {
    HC_LOGW(TAG, "connect to %s:%u failed (state %d)", host_.c_str(), cfg_.port, client_.state());
    return false;
  }
  std::vector<std::string> subs;
  {
    std::lock_guard<std::recursive_mutex> lk(sys_.mutex());
    subs = bridge_.subscriptions();
  }
  for (auto& s : subs) client_.subscribe(s.c_str());
  if (cfg_.discovery) client_.subscribe((cfg_.discoveryPrefix + "/status").c_str());
  HC_LOGI(TAG, "connected to %s", host_.c_str());
  publishAll(cfg_.discovery);
  return true;
}

void MqttLink::publishAll(bool withDiscovery) {
  if (publishing_) return;  // client_.loop() below may deliver "homeassistant/status"
  publishing_ = true;
  std::vector<hc::MqttMsg> msgs;
  {
    std::lock_guard<std::recursive_mutex> lk(sys_.mutex());
    if (withDiscovery) bridge_.discovery(msgs);
    bridge_.state(msgs);
  }
  for (auto& m : msgs) {
    client_.publish(m.topic.c_str(), reinterpret_cast<const uint8_t*>(m.payload.data()), m.payload.size(), m.retain);
    client_.loop();
  }
  lastState_ = millis();
  publishing_ = false;
}

void MqttLink::loop(bool online) {
  bool enabled;
  {
    std::lock_guard<std::recursive_mutex> lk(sys_.mutex());
    enabled = sys_.mqttCfg.enabled;
  }
  enabled_ = enabled;
  up_ = client_.connected();
  if (reconnect_ || !enabled || !online) {
    if (client_.connected()) client_.disconnect();
    reconnect_ = false;
    cfg_.enabled = enabled;
    out_.clear();
    wasConnected_ = false;
    if (!enabled || !online) return;
  }
  uint32_t now = millis();
  if (!client_.connected()) {
    if (wasConnected_) {
      HC_LOGW(TAG, "connection lost");
      wasConnected_ = false;
    }
    if (now - lastAttempt_ < backoffMs_) return;
    lastAttempt_ = now;
    if (!connect()) {
      backoffMs_ = backoffMs_ < 60000 ? backoffMs_ * 2 : 60000;
      return;
    }
    backoffMs_ = 2000;
    wasConnected_ = true;
  }
  client_.loop();
  hc::MqttMsg m;
  for (int i = 0; i < 20 && out_.pop(m); ++i)
    client_.publish(m.topic.c_str(), reinterpret_cast<const uint8_t*>(m.payload.data()), m.payload.size(), m.retain);
  if (now - lastState_ > cfg_.stateIntervalSec * 1000u) publishAll(false);
  // republish discovery when rooms, RF devices, sensors or rules change
  if (cfg_.discovery && now - lastSignatureCheck_ > 10000) {
    lastSignatureCheck_ = now;
    uint32_t sig;
    {
      std::lock_guard<std::recursive_mutex> lk(sys_.mutex());
      sig = sys_.heating.revision() * 31u + sys_.rfReg.revision() * 17u + sys_.sensors.revision() * 7u +
            static_cast<uint32_t>(sys_.automation.rules().size()) * 3u;
      for (auto& r : sys_.automation.rules()) sig += r.enabled ? r.id : 0;
    }
    if (sig != signature_) {
      signature_ = sig;
      publishAll(true);
    }
  }
}

}  // namespace platform
