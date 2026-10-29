#include "platform/Ota.h"

#include <Preferences.h>
#include <Update.h>
#include <esp_ota_ops.h>

#include "hc/core/Log.h"

namespace platform {

namespace {
constexpr const char* TAG = "ota";
constexpr const char* kNvs = "hcota";
// Probation: confirm after the tasks ran healthy for 60 s (at the earliest
// 2 min after boot); reject if that has not happened after 10 min.
constexpr uint32_t kConfirmAfterMs = 120000;
constexpr uint32_t kHealthyForMs = 60000;
constexpr uint32_t kRejectAfterMs = 600000;

std::string nvsGet(const char* key) {
  Preferences p;
  if (!p.begin(kNvs, true)) return std::string();
  String v = p.getString(key, "");
  p.end();
  return v.c_str();
}

void nvsPut(const char* key, const std::string& v) {
  Preferences p;
  if (!p.begin(kNvs, false)) return;
  p.putString(key, v.c_str());
  p.end();
}
}  // namespace

void Ota::setResult(const std::string& r, bool persist) {
  {
    std::lock_guard<std::mutex> lk(mtx_);
    status_.lastResult = r;
  }
  if (persist) nvsPut("res", r);
  HC_LOGI(TAG, "%s", r.c_str());
}

void Ota::refreshSlots() {
  const esp_partition_t* run = esp_ota_get_running_partition();
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  bool avail = false;
  if (other && !busy_) {
    esp_app_desc_t desc;
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    bool stateKnown = esp_ota_get_state_partition(other, &st) == ESP_OK;
    avail = esp_ota_get_partition_description(other, &desc) == ESP_OK &&
            !(stateKnown && (st == ESP_OTA_IMG_INVALID || st == ESP_OTA_IMG_ABORTED));
  }
  std::lock_guard<std::mutex> lk(mtx_);
  status_.runningPartition = run ? run->label : "?";
  status_.slotSize = other ? other->size : 0;
  status_.rollbackAvailable = avail;
}

void Ota::begin() {
  const esp_partition_t* run = esp_ota_get_running_partition();
  esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
  bool pending = run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
  std::string installed = nvsGet("new");  // version written by the last update
  {
    std::lock_guard<std::mutex> lk(mtx_);
    status_.previousVersion = nvsGet("prev");
    status_.lastResult = nvsGet("res");
    status_.pendingVerify = pending;
  }
  probationDone_ = !pending;
  if (!installed.empty() && installed != hc::kFirmwareVersion) {
    // The update was written but this is not it: the bootloader went back.
    setResult("Firmware " + installed + " ist nicht sauber gestartet - automatisch zu " + hc::kFirmwareVersion +
                  " zurueckgekehrt",
              true);
    nvsPut("new", "");
  } else if (!installed.empty() && !pending) {
    nvsPut("new", "");  // running the installed version, nothing left to watch
  }
  if (pending) HC_LOGW(TAG, "new firmware %s on probation (rollback armed)", hc::kFirmwareVersion);
  refreshSlots();
}

std::string Ota::start(size_t expectedSize, const std::string& expectedSha256, const char* source) {
  bool expected = false;
  if (!busy_.compare_exchange_strong(expected, true)) return "Es laeuft bereits ein Update";
  const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
  if (!target) {
    busy_ = false;
    return "keine OTA-Partition";
  }
  if (expectedSize && expectedSize > target->size) {
    busy_ = false;
    return "Firmware groesser als der OTA-Speicher";
  }
  check_.reset();
  expectedSha_ = expectedSha256;
  expectedSize_ = expectedSize;
  source_ = source;
  error_.clear();
  if (!Update.begin(expectedSize ? expectedSize : UPDATE_SIZE_UNKNOWN, U_FLASH)) {
    error_ = Update.errorString();
    busy_ = false;
    return error_;
  }
  {
    std::lock_guard<std::mutex> lk(mtx_);
    status_.busy = true;
    status_.rollbackAvailable = false;  // the other slot is being overwritten
  }
  HC_LOGI(TAG, "update started (%s, %u bytes)", source, static_cast<unsigned>(expectedSize));
  return std::string();
}

bool Ota::write(const uint8_t* data, size_t len) {
  if (!busy_ || !error_.empty()) return false;
  if (!check_.feed(data, len)) {
    error_ = check_.error();
    return false;
  }
  if (Update.write(const_cast<uint8_t*>(data), len) != len) {
    error_ = Update.errorString();
    return false;
  }
  return true;
}

void Ota::abort(const char* why) {
  if (!busy_) return;
  Update.abort();
  if (error_.empty()) error_ = why;
  setResult(std::string("Update abgebrochen: ") + error_, true);
  {
    std::lock_guard<std::mutex> lk(mtx_);
    status_.busy = false;
  }
  busy_ = false;
  refreshSlots();
}

std::string Ota::finish() {
  if (!busy_) return "kein Update aktiv";
  std::string err = error_;
  if (err.empty()) check_.finish(err, expectedSha_, expectedSize_);
  if (!err.empty()) {
    error_ = err;
    abort(err.c_str());
    return err;
  }
  // Update.end validates the image (incl. its appended hash) and selects the
  // slot for the next boot; the bootloader starts it on probation.
  if (!Update.end(true)) {
    error_ = Update.errorString();
    abort(error_.c_str());
    return error_;
  }
  nvsPut("prev", hc::kFirmwareVersion);
  nvsPut("new", check_.version());
  {
    std::lock_guard<std::mutex> lk(mtx_);
    status_.previousVersion = hc::kFirmwareVersion;
    status_.busy = false;
  }
  setResult("Firmware " + check_.version() + " installiert (" + source_ + ", SHA-256 " + check_.sha256().substr(0, 12) +
                "...) - Neustart",
            true);
  busy_ = false;
  return std::string();
}

std::string Ota::rollback() {
  if (busy_) return "Es laeuft ein Update";
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  esp_app_desc_t desc;
  if (!other || esp_ota_get_partition_description(other, &desc) != ESP_OK) return "keine vorherige Firmware vorhanden";
  // esp_ota_set_boot_partition verifies the complete image first
  if (esp_ota_set_boot_partition(other) != ESP_OK) return "vorherige Firmware ist beschaedigt";
  std::string prev;
  {
    std::lock_guard<std::mutex> lk(mtx_);
    prev = status_.previousVersion;
  }
  nvsPut("prev", hc::kFirmwareVersion);
  nvsPut("new", prev);  // a failed start of the old image is reported as well
  setResult("Zurueck zur vorherigen Firmware" + (prev.empty() ? std::string() : " " + prev) + " - Neustart", true);
  return std::string();
}

bool Ota::probation(bool healthy, uint32_t uptimeMs) {
  if (probationDone_) return false;
  if (!healthy) healthySinceMs_ = 0;
  else if (!healthySinceMs_) healthySinceMs_ = uptimeMs ? uptimeMs : 1;
  if (healthySinceMs_ && uptimeMs >= kConfirmAfterMs && uptimeMs - healthySinceMs_ >= kHealthyForMs) {
    probationDone_ = true;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
      nvsPut("new", "");
      {
        std::lock_guard<std::mutex> lk(mtx_);
        status_.pendingVerify = false;
      }
      setResult(std::string("Firmware ") + hc::kFirmwareVersion + " nach Selbstpruefung bestaetigt", true);
      refreshSlots();
    }
    return false;
  }
  if (uptimeMs >= kRejectAfterMs) {
    probationDone_ = true;
    setResult(std::string("Firmware ") + hc::kFirmwareVersion + " hat die Selbstpruefung nicht bestanden - Rueckkehr",
              true);
    return true;
  }
  return false;
}

void Ota::rejectAndRestart() {
  nvsPut("new", "");
  esp_ota_mark_app_invalid_rollback_and_reboot();  // does not return on success
  ESP.restart();
}

void Ota::fillStatus(hc::FirmwareStatus& s) {
  if (!busy_) refreshSlots();
  std::lock_guard<std::mutex> lk(mtx_);
  s = status_;
}

}  // namespace platform
