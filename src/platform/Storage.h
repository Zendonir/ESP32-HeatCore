#pragma once
// Persistent configuration on LittleFS (atomic tmp+rename writes) and an
// optional event log on the SD card.
#include <Arduino.h>

#include <string>

#include "hc/system/HeatCoreSystem.h"

namespace platform {

class Storage {
 public:
  bool begin();
  bool ready() const { return fsOk_; }
  // Reads /cfg/<section>.json into `out`; false if missing or corrupt.
  bool load(hc::ConfigSection s, std::string& out);
  bool save(hc::ConfigSection s, const std::string& json);
  void factoryReset();
  size_t usedBytes() const;
  size_t totalBytes() const;

  // SD card (optional)
  bool sdBegin();
  void sdLog(const std::string& line, const hc::LocalTime& lt);
  std::string sdStatus() const;
  bool sdMounted() const { return sdOk_; }
  uint64_t sdTotal() const;
  uint64_t sdUsed() const;

 private:
  bool fsOk_ = false;
  bool sdOk_ = false;
  uint32_t sdErrors_ = 0;
};

}  // namespace platform
