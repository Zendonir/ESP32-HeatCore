#pragma once
// Firmware identity and update image checks (platform independent).
//
// Every HeatCore build carries the marker "HCFW1|<board>|<version>" in its
// image. An uploaded update is accepted only if it is an ESP32-S3 application
// image (not a merged factory image) and contains the marker of this board.
// The SHA-256 of the transferred image can be checked against the value
// published with a release.
#include <cstddef>
#include <cstdint>
#include <string>

namespace hc {

extern const char* const kFirmwareVersion;  // e.g. "0.5.0" or "0.5.0-dev+1a2b3c4"
extern const char* const kBoardId;          // "KC868-A16V3"
extern const char* const kFirmwareBuildDate;

// Compares release versions "1.2.3", "v1.2.3", "1.2.3-rc1" (build metadata
// after '+' is ignored; a pre-release sorts before its release).
// Returns <0, 0, >0.
int compareVersions(const std::string& a, const std::string& b);

class Sha256 {
 public:
  Sha256() { reset(); }
  void reset();
  void update(const uint8_t* data, size_t len);
  void finish(uint8_t out[32]);
  std::string hex();  // finishes and returns the lowercase hex digest

 private:
  void block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t bufLen_ = 0;
  uint64_t total_ = 0;
};

class FirmwareImageCheck {
 public:
  static constexpr uint16_t kChipEsp32S3 = 0x0009;

  explicit FirmwareImageCheck(const char* board = kBoardId);
  void reset();
  // Returns false as soon as the image is known to be unusable (header).
  bool feed(const uint8_t* data, size_t len);
  // Complete check after the last byte. expectedSha256 / expectedSize are
  // optional (empty / 0).
  bool finish(std::string& error, const std::string& expectedSha256 = std::string(), size_t expectedSize = 0);

  size_t size() const { return size_; }
  const std::string& version() const { return version_; }  // from the marker
  const std::string& sha256() const { return sha_; }        // after finish()
  const std::string& error() const { return error_; }

 private:
  void scan(uint8_t c);

  std::string prefix_;
  int fail_[40] = {};
  Sha256 hash_;
  uint8_t head_[48] = {};
  size_t size_ = 0;
  int match_ = 0;           // matched prefix characters
  bool capturing_ = false;  // prefix matched, reading the version
  std::string capture_;
  std::string version_;
  std::string sha_;
  std::string error_;
};

}  // namespace hc
