#include "hc/ble/BleDecoders.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "hc/ble/BleCrypto.h"

namespace hc {
namespace ble {

namespace {

constexpr uint16_t kUuidBtHome = 0xFCD2;
constexpr uint16_t kUuidEnv = 0x181A;  // pvvx / ATC1441
constexpr uint16_t kUuidMi = 0xFE95;
constexpr uint16_t kUuidSwitchBotOld = 0x0D00;
constexpr uint16_t kUuidSwitchBot = 0xFD3D;
constexpr uint16_t kCompanyGovee = 0xEC88;

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
int16_t sle16(const uint8_t* p) { return static_cast<int16_t>(le16(p)); }
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

// Walks the AD structures: fn(type, data, len) returns false to stop.
template <typename F>
void forEachAd(const uint8_t* adv, size_t len, F fn) {
  size_t i = 0;
  while (i < len) {
    uint8_t l = adv[i];
    if (l == 0 || i + 1 + l > len) return;
    if (!fn(adv[i + 1], adv + i + 2, static_cast<size_t>(l - 1))) return;
    i += 1 + l;
  }
}

bool plausible(const Reading& r) {
  if (r.hasTemp && (!std::isfinite(r.temp) || r.temp < -60.f || r.temp > 100.f)) return false;
  if (r.hasHum && (!std::isfinite(r.hum) || r.hum < 0.f || r.hum > 100.f)) return false;
  return true;
}

// ---- BTHome v2
int btHomeObjectLen(uint8_t id) {
  if (id <= 0x01) return 1;                               // packet id, battery
  if (id >= 0x02 && id <= 0x03) return 2;                 // temperature, humidity
  if (id >= 0x04 && id <= 0x05) return 3;                 // pressure, illuminance
  if (id >= 0x06 && id <= 0x08) return 2;                 // mass, dew point
  if (id == 0x09) return 1;                               // count
  if (id >= 0x0A && id <= 0x0B) return 3;                 // energy, power
  if (id >= 0x0C && id <= 0x0E) return 2;                 // voltage, pm2.5, pm10
  if (id >= 0x0F && id <= 0x11) return 1;                 // binary
  if (id >= 0x12 && id <= 0x14) return 2;                 // co2, tvoc, moisture
  if (id >= 0x15 && id <= 0x2F) return 1;                 // binary sensors, humidity/moisture u8
  if (id == 0x3A) return 1;                               // button
  if (id == 0x3C) return 2;                               // dimmer
  if (id == 0x3D) return 2;
  if (id == 0x3E) return 4;
  if (id >= 0x3F && id <= 0x41) return 2;
  if (id == 0x42) return 3;
  if (id >= 0x43 && id <= 0x45) return 2;                 // current, speed, temperature 0.1
  if (id == 0x46) return 1;
  if (id >= 0x47 && id <= 0x4A) return 2;
  if (id == 0x4B) return 3;
  if (id >= 0x4C && id <= 0x50) return 4;
  if (id >= 0x51 && id <= 0x52) return 2;
  if (id == 0x55) return 4;
  if (id == 0x56) return 2;
  if (id >= 0x57 && id <= 0x59) return 1;                 // temperature s8, s8*0.35, count s8
  if (id == 0x5A) return 2;
  if (id >= 0x5B && id <= 0x5C) return 4;
  if (id >= 0x5D && id <= 0x5F) return 2;
  if (id == 0x60) return 1;
  if (id == 0xF0) return 2;
  if (id == 0xF1) return 4;
  if (id == 0xF2) return 3;
  return -1;  // unknown (0x53/0x54 variable length, future ids): stop
}

void btHomeObjects(const uint8_t* p, size_t n, Reading& r) {
  size_t i = 0;
  while (i < n) {
    uint8_t id = p[i++];
    int l = btHomeObjectLen(id);
    if (l < 0 || i + static_cast<size_t>(l) > n) return;
    const uint8_t* v = p + i;
    switch (id) {
      case 0x00: r.counter = v[0]; break;
      case 0x01: r.batteryPct = v[0]; break;
      case 0x02:
        if (!r.hasTemp) r.temp = sle16(v) / 100.f, r.hasTemp = true;
        break;
      case 0x45:
        if (!r.hasTemp) r.temp = sle16(v) / 10.f, r.hasTemp = true;
        break;
      case 0x57:
        if (!r.hasTemp) r.temp = static_cast<int8_t>(v[0]), r.hasTemp = true;
        break;
      case 0x58:
        if (!r.hasTemp) r.temp = static_cast<int8_t>(v[0]) * 0.35f, r.hasTemp = true;
        break;
      case 0x03:
        if (!r.hasHum) r.hum = le16(v) / 100.f, r.hasHum = true;
        break;
      case 0x2E:
        if (!r.hasHum) r.hum = v[0], r.hasHum = true;
        break;
      default: break;
    }
    i += static_cast<size_t>(l);
  }
}

bool decodeBtHome(const Mac& mac, const uint8_t* d, size_t n, const KeyLookup& keys, Reading& r) {
  if (n < 1) return false;
  uint8_t info = d[0];
  if ((info >> 5) != 2) {  // BTHome v1 uses other UUIDs; unknown versions are skipped
    r.format = Format::BtHome;
    r.unsupported = true;
    return true;
  }
  r.format = Format::BtHome;
  if (!(info & 0x01)) {
    btHomeObjects(d + 1, n - 1, r);
    return true;
  }
  r.encrypted = true;
  if (n < 1 + 8 + 1) return false;  // counter 4 + MIC 4 + at least one byte
  const Key* key = keys ? keys(mac) : nullptr;
  if (!key) {
    r.needsKey = true;
    return true;
  }
  const uint8_t* counter = d + n - 8;
  const uint8_t* mic = d + n - 4;
  uint8_t nonce[13];
  std::memcpy(nonce, mac.data(), 6);
  nonce[6] = static_cast<uint8_t>(kUuidBtHome & 0xFF);
  nonce[7] = static_cast<uint8_t>(kUuidBtHome >> 8);
  nonce[8] = info;
  std::memcpy(nonce + 9, counter, 4);
  size_t ctLen = n - 1 - 8;
  uint8_t plain[32];
  if (ctLen > sizeof(plain)) return false;
  if (!ccmDecrypt(key->data(), nonce, 13, nullptr, 0, d + 1, ctLen, mic, 4, plain)) {
    r.keyError = true;
    return true;
  }
  btHomeObjects(plain, ctLen, r);
  r.counter = static_cast<int64_t>(counter[0]) | static_cast<int64_t>(counter[1]) << 8 |
              static_cast<int64_t>(counter[2]) << 16 | static_cast<int64_t>(counter[3]) << 24;
  return true;
}

// ---- pvvx / ATC1441 (UUID 0x181A)
bool decodeEnv(const uint8_t* d, size_t n, Reading& r) {
  if (n == 15 || n == 16) {  // pvvx: mac le, temp s16 0.01, hum u16 0.01, mV, %, counter, flags
    r.format = Format::Pvvx;
    r.temp = sle16(d + 6) / 100.f;
    r.hum = le16(d + 8) / 100.f;
    r.batteryPct = d[12];
    r.counter = d[13];
    r.hasTemp = r.hasHum = true;
    return true;
  }
  if (n == 13) {  // ATC1441: mac be, temp s16 be 0.1, hum %, battery %, mV be, counter
    r.format = Format::Atc;
    r.temp = static_cast<int16_t>(be16(d + 6)) / 10.f;
    r.hum = d[8];
    r.batteryPct = d[9];
    r.counter = d[12];
    r.hasTemp = r.hasHum = true;
    return true;
  }
  return false;
}

// ---- Xiaomi MiBeacon (UUID 0xFE95)
void miObjects(const uint8_t* p, size_t n, Reading& r) {
  size_t i = 0;
  while (i + 3 <= n) {
    uint16_t type = le16(p + i);
    uint8_t l = p[i + 2];
    i += 3;
    if (i + l > n) return;
    const uint8_t* v = p + i;
    switch (type) {
      case 0x1004:
        if (l == 2) r.temp = sle16(v) / 10.f, r.hasTemp = true;
        break;
      case 0x1006:
        if (l == 2) r.hum = le16(v) / 10.f, r.hasHum = true;
        break;
      case 0x100A:
        if (l >= 1) r.batteryPct = v[0];
        break;
      case 0x100D:
        if (l == 4) {
          r.temp = sle16(v) / 10.f;
          r.hum = le16(v + 2) / 10.f;
          r.hasTemp = r.hasHum = true;
        }
        break;
      case 0x4C01:
        if (l == 4) {
          float f;
          std::memcpy(&f, v, 4);
          r.temp = f;
          r.hasTemp = true;
        }
        break;
      case 0x4C02:
        if (l == 1) r.hum = v[0], r.hasHum = true;
        break;
      case 0x4803:
        if (l == 1) r.batteryPct = v[0];
        break;
      default: break;
    }
    i += l;
  }
}

bool decodeMi(const Mac& advMac, const uint8_t* d, size_t n, const KeyLookup& keys, Reading& r) {
  if (n < 5) return false;
  uint16_t fc = le16(d);
  bool encrypted = fc & 0x0008, hasMac = fc & 0x0010, hasCap = fc & 0x0020, hasObj = fc & 0x0040;
  int version = fc >> 12;
  r.format = Format::MiBeacon;
  r.counter = d[4];
  size_t i = 5;
  Mac mac = advMac;
  if (hasMac) {
    if (i + 6 > n) return false;
    mac = macFromLittleEndian(d + i);
    r.hasDeviceMac = true;
    r.deviceMac = mac;
    i += 6;
  }
  if (hasCap) {
    if (i + 1 > n) return false;
    uint8_t cap = d[i++];
    if (cap & 0x20) ++i;  // I/O capability
  }
  if (!hasObj || i >= n) return true;  // e.g. pairing frames without data
  if (!encrypted) {
    miObjects(d + i, n - i, r);
    return true;
  }
  r.encrypted = true;
  if (version < 4) {  // legacy encryption (v2/v3) is not supported
    r.unsupported = true;
    return true;
  }
  if (n < i + 7 + 1) return false;  // ext counter 3 + MIC 4
  const Key* key = keys ? keys(mac) : nullptr;
  if (!key) {
    r.needsKey = true;
    return true;
  }
  uint8_t nonce[12];
  for (int k = 0; k < 6; ++k) nonce[k] = mac[static_cast<size_t>(5 - k)];
  std::memcpy(nonce + 6, d + 2, 3);       // product id + frame counter
  std::memcpy(nonce + 9, d + n - 7, 3);   // extended counter
  const uint8_t aad = 0x11;
  size_t ctLen = n - 7 - i;
  uint8_t plain[32];
  if (ctLen > sizeof(plain)) return false;
  if (!ccmDecrypt(key->data(), nonce, 12, &aad, 1, d + i, ctLen, d + n - 4, 4, plain)) {
    r.keyError = true;
    return true;
  }
  miObjects(plain, ctLen, r);
  r.counter = static_cast<int64_t>(d[4]) | static_cast<int64_t>(d[n - 7]) << 8 |
              static_cast<int64_t>(d[n - 6]) << 16 | static_cast<int64_t>(d[n - 5]) << 24;
  return true;
}

// ---- Govee (manufacturer data, company id 0xEC88)
bool decodeGovee(const uint8_t* d, size_t n, Reading& r) {
  // d starts after the company id
  if (n == 6 || n == 7) {
    r.format = Format::Govee;
    if (n == 6) {  // H5072 / H5075: 00, packed temp/hum 24 bit BE, battery, ...
      uint32_t v = static_cast<uint32_t>(d[1]) << 16 | static_cast<uint32_t>(d[2]) << 8 | d[3];
      bool neg = v & 0x800000;
      v &= 0x7FFFFF;
      r.temp = static_cast<float>(v / 1000) / 10.f;
      if (neg) r.temp = -r.temp;
      r.hum = static_cast<float>(v % 1000) / 10.f;
      r.batteryPct = d[4];
    } else {  // H5074 / H5051: 00, temp s16 le 0.01, hum u16 le 0.01, battery, ...
      r.temp = sle16(d + 1) / 100.f;
      r.hum = le16(d + 3) / 100.f;
      r.batteryPct = d[5];
    }
    r.hasTemp = r.hasHum = true;
    return true;
  }
  return false;
}

// ---- SwitchBot Meter (service data)
bool decodeSwitchBot(const uint8_t* d, size_t n, Reading& r) {
  if (n < 6) return false;
  uint8_t type = d[0] & 0x7F;
  if (type != 'T' && type != 'i') return false;  // Meter, Meter Plus
  r.format = Format::SwitchBot;
  r.batteryPct = d[2] & 0x7F;
  float t = static_cast<float>(d[4] & 0x7F) + static_cast<float>(d[3] & 0x0F) / 10.f;
  r.temp = (d[4] & 0x80) ? t : -t;
  r.hum = d[5] & 0x7F;
  r.hasTemp = r.hasHum = true;
  return true;
}

}  // namespace

const char* formatName(Format f) {
  switch (f) {
    case Format::BtHome: return "BTHome";
    case Format::Pvvx: return "pvvx";
    case Format::Atc: return "ATC1441";
    case Format::MiBeacon: return "Xiaomi MiBeacon";
    case Format::Govee: return "Govee";
    case Format::SwitchBot: return "SwitchBot";
    case Format::None: break;
  }
  return "-";
}

bool isCandidate(const uint8_t* adv, size_t len) {
  bool hit = false;
  forEachAd(adv, len, [&](uint8_t type, const uint8_t* d, size_t n) {
    if (type == 0x16 && n >= 2) {
      uint16_t u = le16(d);
      hit = u == kUuidBtHome || u == kUuidEnv || u == kUuidMi || u == kUuidSwitchBot || u == kUuidSwitchBotOld;
    } else if (type == 0xFF && n >= 2) {
      hit = le16(d) == kCompanyGovee;
    }
    return !hit;
  });
  return hit;
}

bool decode(const Mac& mac, const uint8_t* adv, size_t len, const KeyLookup& keys, Reading& out) {
  out = Reading();
  bool found = false;
  forEachAd(adv, len, [&](uint8_t type, const uint8_t* d, size_t n) {
    if ((type == 0x08 || type == 0x09) && out.name.empty()) {
      for (size_t k = 0; k < n && k < 24; ++k)
        out.name += std::isprint(d[k]) ? static_cast<char>(d[k]) : '?';
      return true;
    }
    if (found) return true;  // keep scanning for the name only
    if (type == 0x16 && n >= 2) {
      uint16_t u = le16(d);
      Reading r;
      bool ok = false;
      if (u == kUuidBtHome) ok = decodeBtHome(mac, d + 2, n - 2, keys, r);
      else if (u == kUuidEnv) ok = decodeEnv(d + 2, n - 2, r);
      else if (u == kUuidMi) ok = decodeMi(mac, d + 2, n - 2, keys, r);
      else if (u == kUuidSwitchBot || u == kUuidSwitchBotOld) ok = decodeSwitchBot(d + 2, n - 2, r);
      if (ok) {
        std::string name = out.name;
        out = r;
        out.name = name;
        found = true;
      }
    } else if (type == 0xFF && n >= 2 && le16(d) == kCompanyGovee) {
      Reading r;
      if (decodeGovee(d + 2, n - 2, r)) {
        std::string name = out.name;
        out = r;
        out.name = name;
        found = true;
      }
    }
    return true;
  });
  if (!found) return false;
  if (!plausible(out)) {
    out.hasTemp = out.hasHum = false;
    out.unsupported = true;
  }
  if (out.batteryPct > 100) out.batteryPct = 100;
  return true;
}

std::string macHex(const Mac& m) {
  char b[13];
  for (int i = 0; i < 6; ++i) std::snprintf(b + 2 * i, 3, "%02X", m[static_cast<size_t>(i)]);
  return b;
}

namespace {
int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
template <size_t N>
bool parseHex(const std::string& s, std::array<uint8_t, N>& out) {
  std::string h;
  for (char c : s) {
    if (c == ':' || c == '-' || c == ' ') continue;
    if (hexVal(c) < 0) return false;
    h += c;
  }
  if (h.size() != 2 * N) return false;
  for (size_t i = 0; i < N; ++i) out[i] = static_cast<uint8_t>(hexVal(h[2 * i]) << 4 | hexVal(h[2 * i + 1]));
  return true;
}
}  // namespace

bool parseMac(const std::string& s, Mac& out) { return parseHex(s, out); }
bool parseKey(const std::string& hex, Key& out) { return parseHex(hex, out); }

Mac macFromLittleEndian(const uint8_t le[6]) {
  Mac m;
  for (int i = 0; i < 6; ++i) m[static_cast<size_t>(i)] = le[5 - i];
  return m;
}

}  // namespace ble
}  // namespace hc
