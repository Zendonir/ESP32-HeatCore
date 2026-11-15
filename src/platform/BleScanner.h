#pragma once
// Bluetooth LE scanner for thermometer advertisements (NimBLE host of the
// Arduino core). No connection is made; in active mode a scan request asks
// for the scan response with the device name. The radio
// callback only pre-filters and copies candidate advertisements into a queue;
// decoding happens in the HeatCore core (hc::ble::BleHub).
#include <cstdint>
#include <string>

namespace platform {

struct BleAdvert {
  uint8_t mac[6];  // least significant byte first (as received)
  int8_t rssi;
  uint8_t len;
  uint8_t data[31];
};

class BleScanner {
 public:
  static bool supported();
  // Starts the Bluetooth host (blocks until it is ready). Call from a task
  // that is not supervised by the watchdog yet.
  bool begin();
  bool started() const { return started_; }
  // Starts or stops the scan; restarts it after a host reset or when the mode
  // changes. Active: scan requests ask thermometers for their name.
  void setScanning(bool on, bool activeScan = false);
  bool scanning() const;
  bool pop(BleAdvert& a);
  // Stops scanning and shuts the Bluetooth host and controller down,
  // releasing their RAM (firmware update). Bluetooth cannot start again
  // before the next restart.
  void end();
  bool released() const { return released_; }
  uint32_t dropped() const;
  const std::string& error() const { return error_; }

 private:
  bool started_ = false;
  bool activeMode_ = false;
  bool released_ = false;
  std::string error_;
};

}  // namespace platform
