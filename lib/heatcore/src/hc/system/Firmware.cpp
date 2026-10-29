#include "hc/system/Firmware.h"

#include <cstdio>
#include <cstring>

// The PlatformIO build generates include/hc_version.h (tools/version.py)
// with the version from the git tag; host builds use the fallback.
#if defined(__has_include)
#if __has_include("hc_version.h")
#include "hc_version.h"
#endif
#endif
#ifndef HC_FW_VERSION
#define HC_FW_VERSION "0.5.0-dev"
#endif
#define HC_BOARD_ID "KC868-A16V3"
#define HC_FW_MARKER_PREFIX "HCFW1|" HC_BOARD_ID "|"

namespace hc {

// The marker is part of every image; the version string points into it so
// the linker cannot drop it.
extern "C" {
__attribute__((used)) const char hc_fw_marker[] = HC_FW_MARKER_PREFIX HC_FW_VERSION;
}
const char* const kFirmwareVersion = hc_fw_marker + sizeof(HC_FW_MARKER_PREFIX) - 1;
const char* const kBoardId = HC_BOARD_ID;
const char* const kFirmwareBuildDate = __DATE__ " " __TIME__;

// ------------------------------------------------------------ versions

namespace {

struct ParsedVersion {
  long num[4] = {0, 0, 0, 0};
  std::string pre;  // text after '-' (empty = release)
};

ParsedVersion parseVersion(const std::string& s) {
  ParsedVersion v;
  size_t i = (!s.empty() && (s[0] == 'v' || s[0] == 'V')) ? 1 : 0;
  for (int k = 0; k < 4 && i < s.size(); ++k) {
    long n = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') n = n * 10 + (s[i++] - '0');
    v.num[k] = n;
    if (i < s.size() && s[i] == '.') ++i;
    else break;
  }
  if (i < s.size() && s[i] == '-') {
    size_t end = s.find('+', i);
    v.pre = s.substr(i + 1, end == std::string::npos ? std::string::npos : end - i - 1);
  }
  return v;
}

}  // namespace

int compareVersions(const std::string& a, const std::string& b) {
  ParsedVersion x = parseVersion(a), y = parseVersion(b);
  for (int k = 0; k < 4; ++k)
    if (x.num[k] != y.num[k]) return x.num[k] < y.num[k] ? -1 : 1;
  if (x.pre == y.pre) return 0;
  if (x.pre.empty()) return 1;  // release > pre-release
  if (y.pre.empty()) return -1;
  return x.pre < y.pre ? -1 : 1;
}

// ------------------------------------------------------------ SHA-256 (FIPS 180-4)

namespace {
constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
}  // namespace

void Sha256::reset() {
  static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::memcpy(h_, init, sizeof(h_));
  bufLen_ = 0;
  total_ = 0;
}

void Sha256::block(const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) | p[4 * i + 3];
  for (int i = 16; i < 64; ++i) {
    uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
  for (int i = 0; i < 64; ++i) {
    uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
    uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h_[0] += a;
  h_[1] += b;
  h_[2] += c;
  h_[3] += d;
  h_[4] += e;
  h_[5] += f;
  h_[6] += g;
  h_[7] += h;
}

void Sha256::update(const uint8_t* data, size_t len) {
  total_ += len;
  while (len) {
    size_t n = 64 - bufLen_;
    if (n > len) n = len;
    std::memcpy(buf_ + bufLen_, data, n);
    bufLen_ += n;
    data += n;
    len -= n;
    if (bufLen_ == 64) {
      block(buf_);
      bufLen_ = 0;
    }
  }
}

void Sha256::finish(uint8_t out[32]) {
  uint64_t bits = total_ * 8;
  uint8_t pad = 0x80;
  update(&pad, 1);
  uint8_t zero = 0;
  while (bufLen_ != 56) update(&zero, 1);
  uint8_t len[8];
  for (int i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
  update(len, 8);
  for (int i = 0; i < 8; ++i) {
    out[4 * i] = static_cast<uint8_t>(h_[i] >> 24);
    out[4 * i + 1] = static_cast<uint8_t>(h_[i] >> 16);
    out[4 * i + 2] = static_cast<uint8_t>(h_[i] >> 8);
    out[4 * i + 3] = static_cast<uint8_t>(h_[i]);
  }
}

std::string Sha256::hex() {
  uint8_t d[32];
  finish(d);
  char s[65];
  for (int i = 0; i < 32; ++i) std::snprintf(s + 2 * i, 3, "%02x", d[i]);
  return std::string(s, 64);
}

// ------------------------------------------------------------ image check

FirmwareImageCheck::FirmwareImageCheck(const char* board) {
  prefix_ = std::string("HCFW1|") + board + "|";
  if (prefix_.size() > sizeof(fail_) / sizeof(fail_[0])) prefix_.resize(sizeof(fail_) / sizeof(fail_[0]));
  // KMP failure function for the marker prefix
  fail_[0] = 0;
  for (size_t i = 1, k = 0; i < prefix_.size(); ++i) {
    while (k && prefix_[i] != prefix_[k]) k = static_cast<size_t>(fail_[k - 1]);
    if (prefix_[i] == prefix_[k]) ++k;
    fail_[i] = static_cast<int>(k);
  }
  reset();
}

void FirmwareImageCheck::reset() {
  hash_.reset();
  std::memset(head_, 0, sizeof(head_));
  size_ = 0;
  match_ = 0;
  capturing_ = false;
  capture_.clear();
  version_.clear();
  sha_.clear();
  error_.clear();
}

void FirmwareImageCheck::scan(uint8_t c) {
  if (capturing_) {
    bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' ||
              c == '+' || c == '_';
    if (ok && capture_.size() < 31) {
      capture_ += static_cast<char>(c);
      return;
    }
    // A version ends with the string terminator; anything else is not the marker.
    if (c == 0 && !capture_.empty() && version_.empty()) version_ = capture_;
    capturing_ = false;
    capture_.clear();
    match_ = 0;
  }
  while (match_ && static_cast<char>(c) != prefix_[static_cast<size_t>(match_)]) match_ = fail_[match_ - 1];
  if (static_cast<char>(c) == prefix_[static_cast<size_t>(match_)]) ++match_;
  if (match_ == static_cast<int>(prefix_.size())) {
    capturing_ = true;
    match_ = 0;
  }
}

bool FirmwareImageCheck::feed(const uint8_t* data, size_t len) {
  if (!error_.empty()) return false;
  for (size_t i = 0; i < len; ++i) {
    size_t pos = size_ + i;
    if (pos < sizeof(head_)) head_[pos] = data[i];
    if (pos + 1 == sizeof(head_)) {
      uint16_t chip = static_cast<uint16_t>(head_[12] | (head_[13] << 8));
      uint32_t descMagic = head_[32] | (head_[33] << 8) | (head_[34] << 16) | (uint32_t(head_[35]) << 24);
      if (head_[0] != 0xE9) error_ = "keine ESP32-Firmware (Kopf fehlt)";
      else if (chip != kChipEsp32S3) error_ = "Firmware ist nicht fuer den ESP32-S3";
      else if (descMagic != 0xABCD5432u)
        error_ = "kein Anwendungs-Image (Factory-Image? fuer OTA die Datei heatcore-ota.bin verwenden)";
      if (!error_.empty()) return false;
    }
    scan(data[i]);
  }
  hash_.update(data, len);
  size_ += len;
  return true;
}

bool FirmwareImageCheck::finish(std::string& error, const std::string& expectedSha256, size_t expectedSize) {
  if (error_.empty() && size_ < sizeof(head_)) error_ = "Datei zu klein";
  if (error_.empty()) sha_ = hash_.hex();
  if (error_.empty() && version_.empty()) error_ = "keine HeatCore-Firmware fuer " + prefix_.substr(6, prefix_.size() - 7);
  if (error_.empty() && expectedSize && expectedSize != size_) error_ = "Groesse stimmt nicht (Uebertragung unvollstaendig?)";
  if (error_.empty() && !expectedSha256.empty()) {
    std::string e = expectedSha256;
    for (char& ch : e)
      if (ch >= 'A' && ch <= 'F') ch = static_cast<char>(ch - 'A' + 'a');
    if (e != sha_) error_ = "SHA-256-Pruefsumme stimmt nicht";
  }
  error = error_;
  return error_.empty();
}

}  // namespace hc
