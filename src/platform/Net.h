#pragma once
// Network: W5500 Ethernet, Wi-Fi station and the setup access point, mDNS,
// NTP. Which interface runs is decided by hc::NetPolicy (LAN always has
// priority, Wi-Fi only as fallback, the access point only for setup).
// While the access point is up, a DNS server answers every name with the
// controller's address so phones open the setup page automatically
// (captive portal). Heating control never waits for the network.
#include <Arduino.h>
#include <DNSServer.h>

#include <string>
#include <vector>

#include "hc/net/NetPolicy.h"
#include "hc/system/Config.h"
#include "hc/system/HeatCoreSystem.h"

namespace platform {

class NetManager {
 public:
  void begin(const hc::NetworkConfig& cfg, const hc::SystemConfig& sys, const std::string& deviceId);
  void loop();  // call every ~20-100 ms from the network task
  void reconfigure(const hc::NetworkConfig& cfg);
  void startScan();  // asynchronous Wi-Fi scan (results via scanResults)

  bool online() const;  // ETH or Wi-Fi station with IP
  bool apActive() const { return apActive_; }
  bool ethLink() const;
  bool wifiConnected() const;
  int rssi() const;
  std::string ip() const;
  std::string kind() const;   // ETH | WLAN | AP | ""
  std::string detail() const;
  const char* stateText() const { return hc::netStateName(policy_.last().state); }
  std::string apSsid() const { return apActive_ ? apSsid_ : std::string(); }
  bool scanRunning() const { return scanRunning_; }
  const std::vector<hc::WifiNetwork>& scanResults() const { return scan_; }
  bool timeSynced() const { return timeSynced_; }
  void markTimeSynced() { timeSynced_ = true; }

 private:
  bool ethOk() const;
  void apply(const hc::NetDecision& d);
  void startSta();
  void stopSta();
  void startAp();
  void stopAp();
  void connectSta();
  void updateMode();
  void pollScan();

  hc::NetworkConfig cfg_;
  hc::NetPolicy policy_;
  DNSServer dns_;
  std::string deviceId_;
  std::string ntp_, tz_;
  std::string apSsid_;
  bool ethStarted_ = false;
  bool staOn_ = false;
  bool apActive_ = false;
  bool autoReconnect_ = true;
  bool mdnsStarted_ = false;
  bool scanRunning_ = false;
  bool scanRadioTemp_ = false;  // radio switched on only for the scan
  uint32_t scanStartMs_ = 0;
  std::vector<hc::WifiNetwork> scan_;
  bool timeSynced_ = false;
};

}  // namespace platform
