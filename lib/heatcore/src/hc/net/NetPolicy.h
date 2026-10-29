#pragma once
// Which network interfaces should be active (platform independent, tested on
// the host). LAN always has priority:
//
//   LAN connected (link + IP)    -> Wi-Fi station off, access point off
//   no LAN, Wi-Fi configured     -> Wi-Fi station on
//   ... station not connected    -> additionally the setup access point
//   no LAN, no Wi-Fi configured  -> setup access point
//   Wi-Fi connected              -> access point off (after a grace period)
//   LAN back                     -> Wi-Fi off again (after LAN is stable)
//
// Hysteresis avoids flapping: LAN must be stable before Wi-Fi is switched
// off, and must be lost for a while before Wi-Fi starts. While the access
// point is up, station retries are spaced out because each attempt scans
// the channels and interrupts the access point clients.
#include <cstdint>

#include "hc/core/Clock.h"

namespace hc {

struct NetInputs {
  bool ethOk = false;           // LAN link and IP address
  bool wifiConfigured = false;  // Wi-Fi enabled and SSID set
  bool staConnected = false;    // Wi-Fi station associated and has an IP
  bool apClients = false;       // somebody is connected to the access point
};

enum class NetState : uint8_t { Starting = 0, Lan, WifiConnecting, Wifi, Setup };
const char* netStateName(NetState s);  // short German text for the UI

struct NetDecision {
  bool sta = false;        // Wi-Fi station should be running
  bool ap = false;         // setup access point should be running
  bool staRetry = false;   // trigger a new connection attempt now
  bool staAutoReconnect = true;
  NetState state = NetState::Starting;
};

struct NetTiming {
  Millis lanStableMs = 15000;      // LAN up this long -> Wi-Fi off
  Millis lanLostMs = 10000;        // LAN down this long -> Wi-Fi on
  Millis bootGraceMs = 20000;      // time for LAN/DHCP after power-on
  Millis staTimeoutMs = 60000;     // station not connected -> access point
  Millis apOffGraceMs = 60000;     // after connecting: keep AP so the user sees the new address
  Millis apOffGraceClientsMs = 300000;  // ... longer while someone is still connected to the AP
  Millis staRetryInApMs = 180000;  // station retries while the AP is up
};

class NetPolicy {
 public:
  explicit NetPolicy(NetTiming t = NetTiming()) : t_(t) {}
  NetDecision update(const NetInputs& in, Millis now);
  // Wi-Fi credentials changed (e.g. entered via the access point): try at once.
  void credentialsChanged(Millis now);
  const NetDecision& last() const { return d_; }

 private:
  NetTiming t_;
  NetDecision d_;
  bool started_ = false;
  Millis bootMs_ = 0;
  bool lanUp_ = false;        // debounced LAN state
  Millis ethChangeMs_ = 0;    // since when the raw LAN state is as it is
  bool rawEth_ = false;
  Millis staStartMs_ = 0;     // station switched on / last attempt
  Millis onlineSinceMs_ = 0;  // since when LAN or Wi-Fi is connected
  bool online_ = false;
  bool retryNow_ = false;
};

}  // namespace hc
