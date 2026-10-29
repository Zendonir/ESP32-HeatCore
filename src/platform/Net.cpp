#include "platform/Net.h"

#include <ESPmDNS.h>
#include <ETH.h>
#include <WiFi.h>
#include <esp_sntp.h>

#include <algorithm>

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
  apSsid_ = "HeatCore-" + deviceId_.substr(deviceId_.size() > 4 ? deviceId_.size() - 4 : 0);
  WiFi.persistent(false);  // credentials live in HeatCore's configuration only
  WiFi.mode(WIFI_OFF);
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
  sntp_set_time_sync_notification_cb(onSntp);
  configTzTime(tz_.c_str(), ntp_.c_str());
}

bool NetManager::ethOk() const { return ethStarted_ && ETH.linkUp() && ETH.hasIP(); }

void NetManager::updateMode() {
  wifi_mode_t want = staOn_ && apActive_ ? WIFI_AP_STA : staOn_ ? WIFI_STA : apActive_ ? WIFI_AP : WIFI_OFF;
  if (scanRunning_ && want == WIFI_OFF) want = WIFI_STA;  // keep the radio for a running scan
  if (WiFi.getMode() != want) WiFi.mode(want);
}

void NetManager::connectSta() {
  if (cfg_.staticIp)
    WiFi.config(parseIp(cfg_.ip), parseIp(cfg_.gateway), parseIp(cfg_.subnet), parseIp(cfg_.dns));
  WiFi.begin(cfg_.wifiSsid.c_str(), cfg_.wifiPassword.c_str());
}

void NetManager::startSta() {
  WiFi.setHostname(cfg_.hostname.c_str());
  staOn_ = true;
  updateMode();
  WiFi.setAutoReconnect(autoReconnect_);
  connectSta();
  HC_LOGI(TAG, "Wi-Fi station on (%s)", cfg_.wifiSsid.c_str());
}

void NetManager::stopSta() {
  WiFi.disconnect(false, false);
  staOn_ = false;
  updateMode();
  HC_LOGI(TAG, "Wi-Fi station off");
}

void NetManager::startAp() {
  apActive_ = true;
  updateMode();
  if (!WiFi.softAP(apSsid_.c_str(), cfg_.apPassword.c_str())) {
    HC_LOGE(TAG, "access point %s failed", apSsid_.c_str());
    apActive_ = false;
    updateMode();
    return;
  }
  dns_.setErrorReplyCode(DNSReplyCode::NoError);
  dns_.start(53, "*", WiFi.softAPIP());  // captive portal: every name -> 192.168.4.1
  HC_LOGW(TAG, "setup access point %s active (192.168.4.1)", apSsid_.c_str());
}

void NetManager::stopAp() {
  dns_.stop();
  WiFi.softAPdisconnect(false);
  apActive_ = false;
  updateMode();
  HC_LOGI(TAG, "setup access point off");
}

void NetManager::apply(const hc::NetDecision& d) {
  if (d.staAutoReconnect != autoReconnect_) {
    autoReconnect_ = d.staAutoReconnect;
    if (staOn_) WiFi.setAutoReconnect(autoReconnect_);
  }
  if (d.ap && !apActive_) startAp();
  if (d.sta && !staOn_) startSta();
  if (!d.sta && staOn_) stopSta();
  if (!d.ap && apActive_) stopAp();
  if (d.staRetry && staOn_ && WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(false, false);
    connectSta();
  }
}

void NetManager::reconfigure(const hc::NetworkConfig& cfg) {
  bool wifiChanged = cfg.wifiSsid != cfg_.wifiSsid || cfg.wifiPassword != cfg_.wifiPassword || cfg.wifi != cfg_.wifi;
  cfg_ = cfg;
  if (wifiChanged) {
    if (staOn_) {
      WiFi.disconnect(false, false);
      if (cfg_.wifi && !cfg_.wifiSsid.empty()) connectSta();
    }
    policy_.credentialsChanged(millis());
  }
}

void NetManager::startScan() {
  if (scanRunning_) return;
  scanRadioTemp_ = WiFi.getMode() == WIFI_OFF;
  scanRunning_ = true;  // updateMode keeps the radio on while scanning
  updateMode();
  scanStartMs_ = millis();
  if (WiFi.scanNetworks(true, false) == WIFI_SCAN_FAILED) {
    scanRunning_ = false;
    updateMode();
  }
}

void NetManager::pollScan() {
  if (!scanRunning_) return;
  int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING && millis() - scanStartMs_ < 15000) return;
  std::vector<hc::WifiNetwork> list;
  for (int16_t i = 0; i < n; ++i) {
    hc::WifiNetwork w;
    w.ssid = WiFi.SSID(i).c_str();
    if (w.ssid.empty()) continue;  // hidden networks
    w.rssi = WiFi.RSSI(i);
    w.open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    w.channel = WiFi.channel(i);
    auto it = std::find_if(list.begin(), list.end(), [&](const hc::WifiNetwork& x) { return x.ssid == w.ssid; });
    if (it == list.end()) list.push_back(w);
    else if (w.rssi > it->rssi) *it = w;
  }
  WiFi.scanDelete();
  std::sort(list.begin(), list.end(), [](const hc::WifiNetwork& a, const hc::WifiNetwork& b) { return a.rssi > b.rssi; });
  if (list.size() > 20) list.resize(20);
  scan_ = std::move(list);
  scanRunning_ = false;
  if (scanRadioTemp_) updateMode();  // radio off again if LAN is active
  scanRadioTemp_ = false;
}

bool NetManager::ethLink() const { return ethStarted_ && ETH.linkUp(); }
bool NetManager::wifiConnected() const { return staOn_ && WiFi.status() == WL_CONNECTED; }
int NetManager::rssi() const { return wifiConnected() ? WiFi.RSSI() : 0; }

bool NetManager::online() const { return ethOk() || wifiConnected(); }

std::string NetManager::kind() const {
  if (ethOk()) return "ETH";
  if (wifiConnected()) return "WLAN";
  if (apActive_) return "AP";
  return "";
}

std::string NetManager::ip() const {
  if (ethOk()) return ETH.localIP().toString().c_str();
  if (wifiConnected()) return WiFi.localIP().toString().c_str();
  if (apActive_) return WiFi.softAPIP().toString().c_str();
  return "";
}

std::string NetManager::detail() const {
  std::string d;
  if (ethStarted_) d += std::string("LAN ") + (ETH.linkUp() ? (ETH.hasIP() ? "verbunden" : "Link, keine IP") : "kein Kabel");
  if (staOn_)
    d += std::string(d.empty() ? "" : ", ") + "WLAN " +
         (wifiConnected() ? WiFi.localIP().toString().c_str() + std::string(" ") + std::to_string(WiFi.RSSI()) + " dBm"
                          : "verbindet");
  else
    d += std::string(d.empty() ? "" : ", ") + "WLAN aus";
  if (apActive_) d += ", Zugangspunkt " + apSsid_ + " (" + std::to_string(WiFi.softAPgetStationNum()) + " Geraet(e))";
  return d;
}

void NetManager::loop() {
  uint32_t now = millis();
  if (g_sntpSynced) timeSynced_ = true;
  hc::NetInputs in;
  in.ethOk = ethOk();
  in.wifiConfigured = cfg_.wifi && !cfg_.wifiSsid.empty();
  in.staConnected = wifiConnected();
  in.apClients = apActive_ && WiFi.softAPgetStationNum() > 0;
  apply(policy_.update(in, now));
  if (apActive_) dns_.processNextRequest();
  pollScan();
  if (online() && !mdnsStarted_) {
    mdnsStarted_ = MDNS.begin(cfg_.hostname.c_str());
    if (mdnsStarted_) MDNS.addService("http", "tcp", 80);
  }
}

}  // namespace platform
