#pragma once
// 1-Wire protocol layer (Maxim/Analog Devices) for DS18B20-type temperature
// sensors, independent of the hardware: the platform supplies the three
// time-slot primitives (reset/presence, write bit, read bit), everything else
// (ROM search, CRC, commands, temperature decoding) lives here and is tested
// on the host against a simulated bus.
//
// Supported families: 0x28 DS18B20, 0x22 DS1822, 0x42 DS28EA00 (12-bit
// format) and 0x10 DS18S20/DS1820 (9-bit + count remain).
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hc {
namespace ow {

using Rom = std::array<uint8_t, 8>;

class Link {
 public:
  virtual ~Link() = default;
  // Reset pulse; true if at least one device answered with a presence pulse.
  virtual bool reset() = 0;
  virtual void writeBit(bool bit) = 0;
  virtual bool readBit() = 0;
};

enum Cmd : uint8_t {
  kSearchRom = 0xF0,
  kMatchRom = 0x55,
  kSkipRom = 0xCC,
  kConvertT = 0x44,
  kReadScratchpad = 0xBE,
};

uint8_t crc8(const uint8_t* data, size_t len);
void writeByte(Link& l, uint8_t v);
uint8_t readByte(Link& l);

struct SearchResult {
  std::vector<Rom> roms;   // CRC-valid ROM codes in search order
  bool presence = false;   // any device answered the reset
  std::string error;       // empty = complete search
};
// Full ROM search (binary tree walk), at most `max` devices.
SearchResult search(Link& l, size_t max);

bool isTemperatureFamily(uint8_t family);
// Starts a temperature conversion on all devices of the bus (skip ROM).
bool startConversion(Link& l);
// Reads and checks the scratchpad of one device.
bool readScratchpad(Link& l, const Rom& rom, uint8_t sp[9]);
// Decodes a (CRC-checked) scratchpad. False for implausible content.
bool decodeTemperature(uint8_t family, const uint8_t sp[9], float& celsius);
// Power-on value 85 C of the DS18x20 (no conversion happened yet).
bool isPowerOnValue(uint8_t family, const uint8_t sp[9]);

std::string romHex(const Rom& r);

}  // namespace ow
}  // namespace hc
