#include "TestFramework.h"
#include "hc/net/NetPolicy.h"

using namespace hc;

namespace {
// Drives the policy in 1 s steps and returns the last decision.
struct Net {
  NetPolicy p;
  Millis t = 1000;
  NetInputs in;
  NetDecision d;
  NetDecision run(uint32_t seconds) {
    for (uint32_t i = 0; i < seconds; ++i) {
      d = p.update(in, t);
      t += 1000;
    }
    return d;
  }
};
}  // namespace

TEST(net_lan_at_boot_keeps_wifi_and_ap_off) {
  Net n;
  n.in.ethOk = true;
  n.in.wifiConfigured = true;
  n.run(1);
  CHECK(n.d.state == NetState::Lan);
  n.run(600);
  CHECK(!n.d.sta);
  CHECK(!n.d.ap);
}

TEST(net_no_lan_no_wifi_opens_setup_ap) {
  Net n;
  n.run(10);
  CHECK(!n.d.ap);  // boot grace for LAN/DHCP
  n.run(15);
  CHECK(n.d.ap);
  CHECK(!n.d.sta);
  CHECK(n.d.state == NetState::Setup);
  // user enters Wi-Fi credentials via the AP -> station starts at once, AP stays
  n.in.wifiConfigured = true;
  n.p.credentialsChanged(n.t);
  n.run(1);
  CHECK(n.d.sta);
  CHECK(n.d.staRetry);
  CHECK(n.d.ap);
  n.in.staConnected = true;
  n.in.apClients = true;  // the phone is still on the AP
  n.run(30);
  CHECK(n.d.state == NetState::Wifi);
  CHECK(n.d.ap);  // grace: the user can still read the new address
  n.in.apClients = false;
  n.run(31);
  CHECK(!n.d.ap);
  CHECK(n.d.sta);
}

TEST(net_wifi_fallback_and_lan_priority) {
  Net n;
  n.in.ethOk = true;
  n.in.wifiConfigured = true;
  n.run(60);
  CHECK(n.d.state == NetState::Lan && !n.d.sta);
  // cable unplugged: short interruptions are ignored
  n.in.ethOk = false;
  n.run(5);
  CHECK(!n.d.sta);
  n.in.ethOk = true;
  n.run(5);
  CHECK(!n.d.sta);
  // lost for longer -> Wi-Fi station
  n.in.ethOk = false;
  n.run(11);
  CHECK(n.d.sta);
  CHECK(n.d.state == NetState::WifiConnecting);
  CHECK(!n.d.ap);
  n.in.staConnected = true;
  n.run(5);
  CHECK(n.d.state == NetState::Wifi);
  // LAN back: Wi-Fi stays until LAN is stable, then switches off
  n.in.ethOk = true;
  n.run(5);
  CHECK(n.d.sta);
  n.run(11);
  CHECK(n.d.state == NetState::Lan);
  CHECK(!n.d.sta);
  CHECK(!n.d.ap);
}

TEST(net_wifi_unreachable_opens_ap_without_flapping) {
  Net n;
  n.in.wifiConfigured = true;
  n.run(25);
  CHECK(n.d.sta);
  CHECK(!n.d.ap);
  n.run(60);  // station timeout -> setup AP in addition
  CHECK(n.d.ap);
  CHECK(n.d.sta);
  CHECK(!n.d.staAutoReconnect);  // retries spaced out while the AP is up
  CHECK(n.d.state == NetState::Setup);
  int retries = 0;
  bool apAlwaysOn = true;
  for (int i = 0; i < 600; ++i) {
    n.run(1);
    if (n.d.staRetry) ++retries;
    if (!n.d.ap) apAlwaysOn = false;
  }
  CHECK(apAlwaysOn);
  CHECK(retries >= 3 && retries <= 4);  // every 3 min
  // router back: connected -> AP off after the grace period
  n.in.staConnected = true;
  n.run(61);
  CHECK(!n.d.ap);
  CHECK(n.d.staAutoReconnect);
  // LAN plugged in: Wi-Fi off
  n.in.ethOk = true;
  n.run(16);
  CHECK(!n.d.sta);
  CHECK(n.d.state == NetState::Lan);
}

TEST(net_lan_link_without_dhcp_falls_back) {
  Net n;
  n.in.ethOk = false;  // link up but no IP address is reported as not ok
  n.run(30);
  CHECK(n.d.ap);
  n.in.ethOk = true;  // DHCP finally answers
  n.run(16);
  CHECK(n.d.state == NetState::Lan);
  n.run(61);
  CHECK(!n.d.ap);
}

#include "SimRig.h"
#include "hc/api/ApiRouter.h"

TEST(api_setup_wifi_only) {
  sim::Rig r;
  ApiRouter api(r.sys);
  auto call = [&](const char* m, const char* path, const char* body = "") {
    ApiRequest q;
    q.method = m;
    q.path = path;
    q.body = body;
    return api.handle(q);
  };
  std::string cmd;
  r.sys.platformCommand = [&](const std::string& c, JsonObjectConst) {
    cmd = c;
    return std::string();
  };
  r.sys.platform.wifiScan = {{"Heimnetz", -50, false, 6}};
  ApiResponse st = call("GET", "/api/setup/status");
  CHECK_EQ(st.status, 200);
  CHECK(st.body.find("Heimnetz") != std::string::npos);
  CHECK(st.body.find("\"detail\"") == std::string::npos);  // no installation details for AP clients
  CHECK_EQ(call("POST", "/api/setup/scan").status, 200);
  CHECK_STR(cmd, "wifi_scan");
  // validation
  CHECK_EQ(call("POST", "/api/setup/wifi", R"({"ssid":"","password":"geheim123"})").status, 400);
  CHECK_EQ(call("POST", "/api/setup/wifi", R"({"ssid":"Heimnetz","password":"kurz"})").status, 400);
  CHECK(r.sys.netCfg.wifiSsid.empty());
  // valid: only Wi-Fi fields change, the change is applied at once
  std::string apPw = r.sys.netCfg.apPassword;
  r.sys.netCfg.wifi = false;
  CHECK_EQ(call("POST", "/api/setup/wifi", R"({"ssid":"Heimnetz","password":"geheim123"})").status, 200);
  CHECK_STR(r.sys.netCfg.wifiSsid, "Heimnetz");
  CHECK_STR(r.sys.netCfg.wifiPassword, "geheim123");
  CHECK(r.sys.netCfg.wifi);
  CHECK_STR(r.sys.netCfg.apPassword, apPw);
  CHECK_STR(cmd, "apply_network");
  // open network without password
  CHECK_EQ(call("POST", "/api/setup/wifi", R"({"ssid":"Cafe","password":""})").status, 200);
  CHECK(r.sys.netCfg.wifiPassword.empty());
  CHECK_EQ(call("POST", "/api/setup/other").status, 404);
}
