#pragma once
// Firmware update into the inactive OTA slot.
//
// * Every image is checked while it streams in (ESP32-S3 application image,
//   HeatCore marker of this board, optional SHA-256 and size from a release).
// * After an update the new firmware runs on probation: the bootloader's
//   rollback is armed and the image is only confirmed once the control and
//   network tasks have run healthy for a while. A firmware that crashes or
//   hangs before that returns to the previous version automatically.
// * The previous version stays in the other slot and can be restored from
//   the web interface.
// Heating keeps running during the transfer; the restart goes through the
// normal safe shutdown (all valves to the safe state first).
#include <Arduino.h>

#include <atomic>
#include <mutex>
#include <string>

#include "hc/system/Firmware.h"
#include "hc/system/HeatCoreSystem.h"

namespace platform {

class Ota {
 public:
  void begin();  // early in setup(): slot state, probation, last result

  // Upload API (called from the web server task, one transfer at a time).
  std::string start(size_t expectedSize, const std::string& expectedSha256, const char* source);
  bool write(const uint8_t* data, size_t len);
  std::string finish();  // "" = image written and selected for the next boot
  void abort(const char* why);
  bool busy() const { return busy_; }
  std::string lastError() const { return error_; }

  // Switches the boot slot back to the previous firmware ("" = ok).
  std::string rollback();

  // Probation: call regularly with the current health verdict. Returns true
  // when the image was rejected and the caller must restart (safe outputs
  // first) so that the bootloader returns to the previous firmware.
  bool probation(bool healthy, uint32_t uptimeMs);
  void rejectAndRestart();

  void fillStatus(hc::FirmwareStatus& s);

 private:
  void setResult(const std::string& r, bool persist);
  void refreshSlots();

  mutable std::mutex mtx_;
  std::atomic<bool> busy_{false};
  hc::FirmwareImageCheck check_;
  std::string expectedSha_;
  size_t expectedSize_ = 0;
  std::string source_;
  std::string error_;
  hc::FirmwareStatus status_;
  uint32_t healthySinceMs_ = 0;
  bool probationDone_ = true;
};

}  // namespace platform
