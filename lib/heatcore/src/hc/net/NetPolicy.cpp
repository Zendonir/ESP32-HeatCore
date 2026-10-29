#include "hc/net/NetPolicy.h"

namespace hc {

const char* netStateName(NetState s) {
  switch (s) {
    case NetState::Lan: return "LAN aktiv, WLAN aus";
    case NetState::WifiConnecting: return "kein LAN - WLAN verbindet";
    case NetState::Wifi: return "kein LAN - WLAN aktiv";
    case NetState::Setup: return "Einrichtung ueber Zugangspunkt";
    default: return "Start";
  }
}

void NetPolicy::credentialsChanged(Millis now) {
  retryNow_ = true;
  staStartMs_ = now;
}

NetDecision NetPolicy::update(const NetInputs& in, Millis now) {
  if (!started_) {
    started_ = true;
    bootMs_ = now;
    ethChangeMs_ = now;
    rawEth_ = in.ethOk;
  }
  // debounce LAN: stable before it takes over, lost for a while before Wi-Fi starts
  if (in.ethOk != rawEth_) {
    rawEth_ = in.ethOk;
    ethChangeMs_ = now;
  }
  if (rawEth_ && !lanUp_ && (elapsed(now, ethChangeMs_) >= t_.lanStableMs || d_.state == NetState::Starting))
    lanUp_ = true;
  if (!rawEth_ && lanUp_ && elapsed(now, ethChangeMs_) >= t_.lanLostMs) lanUp_ = false;

  NetDecision d;
  d.staRetry = false;
  bool booting = elapsed(now, bootMs_) < t_.bootGraceMs;

  if (lanUp_) {
    d.state = NetState::Lan;
    d.sta = false;
  } else if (booting && !in.staConnected) {
    // give LAN and DHCP a moment after power-on before anything else starts
    d.state = NetState::Starting;
    d.sta = in.wifiConfigured && !rawEth_;
  } else if (in.wifiConfigured) {
    d.sta = true;
    d.state = in.staConnected ? NetState::Wifi : NetState::WifiConnecting;
  } else {
    d.state = NetState::Setup;
  }
  if (rawEth_ && !lanUp_ && d.state != NetState::Lan) {
    // LAN link is coming up: keep the current Wi-Fi state until it is stable
    d.sta = d_.sta && in.wifiConfigured;
  }

  // station start / timeout bookkeeping
  if (d.sta && !d_.sta) staStartMs_ = now;
  bool staTimedOut = d.sta && !in.staConnected && elapsed(now, staStartMs_) >= t_.staTimeoutMs;

  // online tracking for the access point grace period
  bool online = lanUp_ || in.staConnected;
  if (online && !online_) onlineSinceMs_ = now;
  online_ = online;

  // access point
  bool needAp = !online && !booting && (!in.wifiConfigured || staTimedOut || d.state == NetState::Setup);
  if (d_.ap && !online) needAp = true;  // stays up until a connection exists (no flapping between retries)
  if (needAp) {
    d.ap = true;
  } else if (d_.ap && online) {
    Millis grace = in.apClients ? t_.apOffGraceClientsMs : t_.apOffGraceMs;
    d.ap = elapsed(now, onlineSinceMs_) < grace;
  } else {
    d.ap = false;
  }
  if (d.ap && !online && d.state != NetState::Lan) d.state = NetState::Setup;

  // station retries: continuous without AP, spaced out while the AP serves clients
  d.staAutoReconnect = !d.ap;
  if (d.sta && !in.staConnected) {
    if (retryNow_) {
      d.staRetry = true;
      staStartMs_ = now;
    } else if (d.ap && elapsed(now, staStartMs_) >= t_.staRetryInApMs) {
      d.staRetry = true;
      staStartMs_ = now;
    }
  }
  retryNow_ = false;
  d_ = d;
  return d;
}

}  // namespace hc
