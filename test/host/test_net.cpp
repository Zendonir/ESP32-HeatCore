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
