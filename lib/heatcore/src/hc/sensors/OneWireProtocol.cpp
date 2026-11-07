#include "hc/sensors/OneWireProtocol.h"

#include <cstdio>

namespace hc {
namespace ow {

uint8_t crc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; ++i) {
    uint8_t b = data[i];
    for (int k = 0; k < 8; ++k) {
      uint8_t mix = (crc ^ b) & 0x01;
      crc >>= 1;
      if (mix) crc ^= 0x8C;
      b >>= 1;
    }
  }
  return crc;
}

void writeByte(Link& l, uint8_t v) {
  for (int i = 0; i < 8; ++i) l.writeBit((v >> i) & 1);  // LSB first
}

uint8_t readByte(Link& l) {
  uint8_t v = 0;
  for (int i = 0; i < 8; ++i)
    if (l.readBit()) v |= static_cast<uint8_t>(1u << i);
  return v;
}

SearchResult search(Link& l, size_t max) {
  SearchResult res;
  Rom rom{};
  int lastDiscrepancy = 0;  // 1-based bit position, 0 = none
  bool lastDevice = false;
  while (!lastDevice && res.roms.size() < max) {
    if (!l.reset()) {
      if (res.roms.empty() && !res.presence) return res;  // empty bus
      res.error = "Geraet antwortet nicht mehr";
      return res;
    }
    res.presence = true;
    writeByte(l, kSearchRom);
    int lastZero = 0;
    for (int id = 1; id <= 64; ++id) {
      bool bit = l.readBit();
      bool cmp = l.readBit();
      if (bit && cmp) {
        res.error = "keine Antwort bei der Suche (Leitung/Pull-up pruefen)";
        return res;
      }
      bool dir;
      if (bit != cmp) {
        dir = bit;  // all remaining devices agree
      } else {      // discrepancy: both 0 and 1 present
        if (id < lastDiscrepancy) dir = (rom[(id - 1) / 8] >> ((id - 1) % 8)) & 1;
        else dir = id == lastDiscrepancy;
        if (!dir) lastZero = id;
      }
      uint8_t mask = static_cast<uint8_t>(1u << ((id - 1) % 8));
      if (dir) rom[(id - 1) / 8] |= mask;
      else rom[(id - 1) / 8] &= static_cast<uint8_t>(~mask);
      l.writeBit(dir);
    }
    lastDiscrepancy = lastZero;
    if (lastDiscrepancy == 0) lastDevice = true;
    if (rom[0] == 0 || crc8(rom.data(), 7) != rom[7]) {
      res.error = "CRC-Fehler im ROM-Code (Stoerung auf der Leitung?)";
      return res;
    }
    // a disturbed bit can fake a discrepancy and lead to the same device twice
    bool dup = false;
    for (auto& x : res.roms) dup = dup || x == rom;
    if (!dup) res.roms.push_back(rom);
  }
  return res;
}

bool isTemperatureFamily(uint8_t f) { return f == 0x28 || f == 0x22 || f == 0x42 || f == 0x10; }

bool startConversion(Link& l) {
  if (!l.reset()) return false;
  writeByte(l, kSkipRom);
  writeByte(l, kConvertT);
  return true;
}

bool readScratchpad(Link& l, const Rom& rom, uint8_t sp[9]) {
  if (!l.reset()) return false;
  writeByte(l, kMatchRom);
  for (uint8_t b : rom) writeByte(l, b);
  writeByte(l, kReadScratchpad);
  bool allZero = true, allOnes = true;
  for (int i = 0; i < 9; ++i) {
    sp[i] = readByte(l);
    if (sp[i] != 0x00) allZero = false;
    if (sp[i] != 0xFF) allOnes = false;
  }
  // all zero passes the CRC: a shorted data line looks exactly like that
  if (allZero || allOnes) return false;
  return crc8(sp, 8) == sp[8];
}

bool decodeTemperature(uint8_t family, const uint8_t sp[9], float& c) {
  int16_t raw = static_cast<int16_t>(static_cast<uint16_t>(sp[1]) << 8 | sp[0]);
  if (family == 0x10) {
    // DS18S20: 0.5 K steps, extended with COUNT_REMAIN / COUNT_PER_C
    float t = static_cast<float>(raw >> 1);
    if (sp[7] != 0) t = t - 0.25f + static_cast<float>(sp[7] - sp[6]) / static_cast<float>(sp[7]);
    c = t;
  } else {
    // resolution bits R1:R0 in the configuration register; undefined LSBs
    switch ((sp[4] >> 5) & 0x03) {
      case 0: raw = static_cast<int16_t>(raw & ~7); break;  // 9 bit
      case 1: raw = static_cast<int16_t>(raw & ~3); break;  // 10 bit
      case 2: raw = static_cast<int16_t>(raw & ~1); break;  // 11 bit
      default: break;                                        // 12 bit
    }
    c = static_cast<float>(raw) / 16.f;
  }
  return c >= -55.f && c <= 125.f;
}

bool isPowerOnValue(uint8_t family, const uint8_t sp[9]) {
  if (family == 0x10) return sp[0] == 0xAA && sp[1] == 0x00;
  return sp[0] == 0x50 && sp[1] == 0x05;
}

std::string romHex(const Rom& r) {
  char b[17];
  for (int i = 0; i < 8; ++i) std::snprintf(b + 2 * i, 3, "%02X", r[static_cast<size_t>(i)]);
  return b;
}

}  // namespace ow
}  // namespace hc
