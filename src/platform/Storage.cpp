#include "platform/Storage.h"

#include <FS.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>

#include "hc/core/Log.h"
#include "platform/Board.h"

namespace platform {

namespace {
constexpr const char* TAG = "storage";
SPIClass g_sdSpi(HSPI);  // SPI3; SPI2 is used by the W5500

String pathFor(hc::ConfigSection s) { return String("/cfg/") + hc::configSectionName(s) + ".json"; }
}  // namespace

bool Storage::begin() {
  fsOk_ = LittleFS.begin(true);  // formats on first boot
  if (fsOk_ && !LittleFS.exists("/cfg")) LittleFS.mkdir("/cfg");
  if (!fsOk_) HC_LOGE(TAG, "LittleFS mount failed");
  return fsOk_;
}

bool Storage::loadFile(const char* path, std::string& out) {
  if (!fsOk_) return false;
  std::lock_guard<std::mutex> lk(fsMutex_);
  // An interrupted save leaves the .tmp file: the previous version stays valid.
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  out.clear();
  out.reserve(f.size());
  while (f.available()) {
    char buf[512];
    size_t n = f.readBytes(buf, sizeof(buf));
    out.append(buf, n);
  }
  f.close();
  return !out.empty();
}

bool Storage::saveFile(const char* path, const std::string& data) {
  if (!fsOk_) return false;
  std::lock_guard<std::mutex> lk(fsMutex_);
  String p = path;
  String tmp = p + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  size_t n = f.write(reinterpret_cast<const uint8_t*>(data.data()), data.size());
  f.close();
  if (n != data.size()) {
    LittleFS.remove(tmp);
    HC_LOGE(TAG, "write %s incomplete", path);
    return false;
  }
  LittleFS.remove(p);
  bool ok = LittleFS.rename(tmp, p);
  if (!ok) HC_LOGE(TAG, "rename %s failed", path);
  return ok;
}

bool Storage::load(hc::ConfigSection s, std::string& out) { return loadFile(pathFor(s).c_str(), out); }

bool Storage::save(hc::ConfigSection s, const std::string& json) { return saveFile(pathFor(s).c_str(), json); }

void Storage::factoryReset() {
  if (!fsOk_) return;
  for (int i = 0; i < static_cast<int>(hc::ConfigSection::Count); ++i)
    LittleFS.remove(pathFor(static_cast<hc::ConfigSection>(i)));
  LittleFS.remove("/hist.bin");
}

size_t Storage::usedBytes() const { return fsOk_ ? LittleFS.usedBytes() : 0; }
size_t Storage::totalBytes() const { return fsOk_ ? LittleFS.totalBytes() : 0; }

bool Storage::sdBegin() {
  pinMode(board::kSdDetect, INPUT_PULLUP);
  g_sdSpi.begin(board::kSdSck, board::kSdMiso, board::kSdMosi, board::kSdCs);
  sdOk_ = SD.begin(board::kSdCs, g_sdSpi, 20000000);
  if (sdOk_ && !SD.exists("/log")) SD.mkdir("/log");
  return sdOk_;
}

void Storage::sdLog(const std::string& line, const hc::LocalTime& lt) {
  if (!sdOk_) return;
  char name[32];
  if (lt.valid) std::snprintf(name, sizeof(name), "/log/%04d%02d%02d.csv", lt.year, lt.month, lt.day);
  else std::snprintf(name, sizeof(name), "/log/nodate.csv");
  File f = SD.open(name, FILE_APPEND);
  if (!f) {
    if (++sdErrors_ > 10) sdOk_ = false;  // card removed or broken: stop trying
    return;
  }
  f.print(line.c_str());
  f.print('\n');
  f.close();
}

std::string Storage::sdStatus() const {
  if (!sdOk_) return "keine Karte";
  return "ok";
}

uint64_t Storage::sdTotal() const { return sdOk_ ? SD.totalBytes() : 0; }
uint64_t Storage::sdUsed() const { return sdOk_ ? SD.usedBytes() : 0; }

}  // namespace platform
