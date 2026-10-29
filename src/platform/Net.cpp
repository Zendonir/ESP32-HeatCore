#include "platform/Net.h"

#include <ESPmDNS.h>
#include <ETH.h>
#include <WiFi.h>
#include <esp_sntp.h>

#include "hc/core/Log.h"
#include "platform/Board.h"

namespace platform {

namespace {
constexpr const char* TAG = "net";
volatile bool g_sntpSynced = false;
void onSntp(struct timeval*) { g_sntpSynced = true; }

IPAddress parseIp(const std::string& s) {
  IPAddress ip;
  ip.fromString(s.c_str());
  return ip;
}
}  // namespace

void NetManager::begin(const hc::NetworkConfig& cfg, const hc::SystemConfig& sys, const std::string& deviceId) {
  cfg_ = cfg;
  deviceId_ = deviceId;
  ntp_ = sys.ntpServer;
  tz_ = sys.timezone;
  if (cfg_.ethernet) {
    ethStarted_ = ETH.begin(ETH_PHY_W5500, 1, board::kEthCs, board::kEthInt, board::kEthRst, SPI2_HOST, board::kEthSck,
                            board::kEthMiso, board::kEthMosi);
    if (ethStarted_) {
      ETH.setHostname(cfg_.hostname.c_str());
      if (cfg_.staticIp)
        ETH.config(parseIp(cfg_.ip), parseIp(cfg_.gateway), parseIp(cfg_.subnet), parseIp(cfg_.dns));
    } else {
      HC_LOGW(TAG, "W5500 Ethernet not started");
    }
  }
  if (cfg_.wifi && !cfg_.wifiSsid.empty()) startWifi();
  sntp_set_time_sync_notification_cb(onSntp);
  configTzTime(tz_.c_str(), ntp_.c_str());
  offlineSince_ = millis();
}

void NetManager::startWifi() {
  WiFi.persistent(false);
  WiFi.mode(apActive_ ? WIFI_AP_STA : WIFI_STA);
  WiFi.setHostname(cfg_.hostname.c_str());
  if (cfg_.staticIp)
    WiFi.config(parseIp(cfg_.ip), parseIp(cfg_.gateway), parseIp(cfg_.subnet), parseIp(cfg_.dns));
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg_.wifiSsid.c_str(), cfg_.wifiPassword.c_str());
  wifiStarted_ = true;
  lastWifiRetry_ = millis();
}

void NetManager::startAp() {
  std::string ssid = "HeatCore-" + deviceId_.substr(deviceId_.size() > 4 ? deviceId_.size() - 4 : 0);
  WiFi.mode(wifiStarted_ ? WIFI_AP_STA : WIFI_AP);
  apActive_ = WiFi.softAP(ssid.c_str(), cfg_.apPassword.c_str());
  HC_LOGW(TAG, "no network - access point %s %s", ssid.c_str(), apActive_ ? "active (192.168.4.1)" : "failed");
}

void NetManager::reconfigure(const hc::NetworkConfig& cfg) {
  bool wifiChanged = cfg.wifiSsid != cfg_.wifiSsid || cfg.wifiPassword != cfg_.wifiPassword || cfg.wifi != cfg_.wifi;
  cfg_ = cfg;
  if (wifiChanged) {
    WiFi.disconnect(true);
    wifiStarted_ = false;
    if (cfg_.wifi && !cfg_.wifiSsid.empty()) startWifi();
  }
}

bool NetManager::ethLink() const { return ethStarted_ && ETH.linkUp(); }
bool NetManager::wifiConnected() const { return wifiStarted_ && WiFi.status() == WL_CONNECTED; }
int NetManager::rssi() const { return wifiConnected() ? WiFi.RSSI() : 0; }

bool NetManager::online() const {
  return (ethStarted_ && ETH.linkUp() && ETH.hasIP()) || wifiConnected();
}

std::string NetManager::kind() const {
  if (ethStarted_ && ETH.linkUp() && ETH.hasIP()) return "ETH";
  if (wifiConnected()) return "WLAN";
  if (apActive_) return "AP";
  return "";
}

std::string NetManager::ip() const {
  if (ethStarted_ && ETH.linkUp() && ETH.hasIP()) return ETH.localIP().toString().c_str();
  if (wifiConnected()) return WiFi.localIP().toString().c_str();
  if (apActive_) return WiFi.softAPIP().toString().c_str();
  return "";
}

std::string NetManager::detail() const {
  std::string d;
  if (ethStarted_) d += std::string("ETH ") + (ETH.linkUp() ? "Link" : "kein Link");
  if (wifiStarted_) d += std::string(d.empty() ? "" : ", ") + "WLAN " + (wifiConnected() ? std::to_string(WiFi.RSSI()) + " dBm" : "getrennt");
  if (apActive_) d += std::string(d.empty() ? "" : ", ") + "AP";
  return d;
}

void NetManager::loop() {
  uint32_t now = millis();
  if (g_sntpSynced) timeSynced_ = true;
  if (online()) {
    offlineSince_ = now;
    if (!mdnsStarted_) {
      mdnsStarted_ = MDNS.begin(cfg_.hostname.c_str());
      if (mdnsStarted_) MDNS.addService("http", "tcp", 80);
    }
    // keep the access point for a grace period, then switch it off
    if (!onlineSince_) onlineSince_ = now;
    if (apActive_ && now - onlineSince_ > 120000) {
      WiFi.softAPdisconnect(true);
      apActive_ = false;
    }
    return;
  }
  onlineSince_ = 0;
  if (!apActive_ && now - offlineSince_ > 60000) startAp();
  if (wifiStarted_ && !wifiConnected() && now - lastWifiRetry_ > 30000) {
    lastWifiRetry_ = now;
    WiFi.reconnect();
  }
}

}  // namespace platform
