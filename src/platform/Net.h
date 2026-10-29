#pragma once
// Network: W5500 Ethernet (preferred), Wi-Fi station and a fallback access
// point, mDNS and NTP. Heating control never waits for the network.
#include <Arduino.h>

#include <string>

#include "hc/system/Config.h"

namespace platform {

class NetManager {
 public:
  void begin(const hc::NetworkConfig& cfg, const hc::SystemConfig& sys, const std::string& deviceId);
  void loop();  // call every ~100 ms from the network task
  void reconfigure(const hc::NetworkConfig& cfg);

  bool online() const;  // ETH or Wi-Fi station with IP
  bool apActive() const { return apActive_; }
  bool ethLink() const;
  bool wifiConnected() const;
  int rssi() const;
  std::string ip() const;
  std::string kind() const;   // ETH | WLAN | AP | ""
  std::string detail() const;
  bool timeSynced() const { return timeSynced_; }
  void markTimeSynced() { timeSynced_ = true; }

 private:
  void startWifi();
  void startAp();
  hc::NetworkConfig cfg_;
  std::string deviceId_;
  std::string ntp_, tz_;
  bool ethStarted_ = false;
  bool wifiStarted_ = false;
  bool apActive_ = false;
  bool mdnsStarted_ = false;
  uint32_t offlineSince_ = 0;
  uint32_t onlineSince_ = 0;
  uint32_t lastWifiRetry_ = 0;
  bool timeSynced_ = false;
};

}  // namespace platform
