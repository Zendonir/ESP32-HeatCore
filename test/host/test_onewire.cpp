// 1-Wire protocol layer against a bit-level simulated bus with DS18x20s.
#include <cmath>
#include <cstring>

#include "TestFramework.h"
#include "hc/sensors/OneWireProtocol.h"

using namespace hc;

namespace {

struct SimDevice {
  ow::Rom rom{};
  uint8_t sp[9] = {0x50, 0x05, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0};  // power-on 85 C
  float temp = 21.5f;
  bool active = false;
  void setCrc() { sp[8] = ow::crc8(sp, 8); }
  void convert() {
    if (rom[0] == 0x10) {
      // 0.5 K register plus COUNT_REMAIN for the fraction (datasheet formula)
      int16_t half = static_cast<int16_t>(std::floor(temp * 2.f));
      sp[0] = static_cast<uint8_t>(half & 0xFF);
      sp[1] = static_cast<uint8_t>((half >> 8) & 0xFF);
      float frac = temp - static_cast<float>(half >> 1);
      sp[6] = static_cast<uint8_t>(std::lround(16.f - (frac + 0.25f) * 16.f));
      sp[7] = 0x10;
    } else {
      int16_t raw = static_cast<int16_t>(std::lround(temp * 16.f));
      sp[0] = static_cast<uint8_t>(raw & 0xFF);
      sp[1] = static_cast<uint8_t>((raw >> 8) & 0xFF);
    }
    setCrc();
  }
};

ow::Rom makeRom(uint8_t family, uint64_t serial) {
  ow::Rom r{};
  r[0] = family;
  for (int i = 1; i < 7; ++i) r[static_cast<size_t>(i)] = static_cast<uint8_t>(serial >> (8 * (i - 1)));
  r[7] = ow::crc8(r.data(), 7);
  return r;
}

// Wired-AND bus: every device pulls the line low for a 0.
class SimBus : public ow::Link {
 public:
  std::vector<SimDevice> devs;
  bool shorted = false;     // data line stuck low
  int corruptBit = -1;      // flips this read bit (counted from reset) once
  int resets = 0;

  bool reset() override {
    ++resets;
    state_ = State::RomCmd;
    bits_ = 0;
    nbits_ = 0;
    readCount_ = 0;
    for (auto& d : devs) d.active = true;
    return shorted || !devs.empty();
  }
  void writeBit(bool b) override {
    if (shorted) return;
    if (state_ == State::SearchWrite) {
      for (auto& d : devs)
        if (d.active && romBit(d, searchPos_) != b) d.active = false;
      if (++searchPos_ == 64) state_ = State::Idle;
      else state_ = State::SearchRead1;
      return;
    }
    if (state_ == State::MatchRom) {
      for (auto& d : devs)
        if (d.active && romBit(d, matchPos_) != b) d.active = false;
      if (++matchPos_ == 64) state_ = State::FuncCmd;
      return;
    }
    bits_ |= static_cast<uint8_t>(b ? 1u << nbits_ : 0u);
    if (++nbits_ < 8) return;
    uint8_t cmd = bits_;
    bits_ = 0;
    nbits_ = 0;
    if (state_ == State::RomCmd) {
      if (cmd == ow::kSearchRom) {
        state_ = State::SearchRead1;
        searchPos_ = 0;
      } else if (cmd == ow::kSkipRom) {
        state_ = State::FuncCmd;
      } else if (cmd == ow::kMatchRom) {
        state_ = State::MatchRom;
        matchPos_ = 0;
      }
    } else if (state_ == State::FuncCmd) {
      if (cmd == ow::kConvertT) {
        for (auto& d : devs)
          if (d.active) d.convert();
        state_ = State::Idle;
      } else if (cmd == ow::kReadScratchpad) {
        state_ = State::ReadSp;
        spPos_ = 0;
      }
    }
  }
  bool readBit() override {
    bool v = readRaw();
    if (readCount_++ == corruptBit) {
      corruptBit = -1;
      v = !v;
    }
    return v;
  }

 private:
  enum class State { Idle, RomCmd, FuncCmd, SearchRead1, SearchRead2, SearchWrite, MatchRom, ReadSp };
  State state_ = State::Idle;
  uint8_t bits_ = 0;
  int nbits_ = 0, searchPos_ = 0, matchPos_ = 0, spPos_ = 0, readCount_ = 0;

  static bool romBit(const SimDevice& d, int pos) { return (d.rom[static_cast<size_t>(pos / 8)] >> (pos % 8)) & 1; }
  bool readRaw() {
    if (shorted) return false;
    bool line = true;
    if (state_ == State::SearchRead1 || state_ == State::SearchRead2) {
      bool comp = state_ == State::SearchRead2;
      for (auto& d : devs)
        if (d.active && (romBit(d, searchPos_) != comp) == false) line = false;
      state_ = comp ? State::SearchWrite : State::SearchRead2;
      return line;
    }
    if (state_ == State::ReadSp) {
      int byte = spPos_ / 8, bit = spPos_ % 8;
      ++spPos_;
      if (byte >= 9) return true;
      for (auto& d : devs)
        if (d.active && !((d.sp[byte] >> bit) & 1)) line = false;
      return line;
    }
    return true;
  }
};

}  // namespace

TEST(onewire_crc8_known_vector) {
  // ROM code from the DS18B20 datasheet example family 0x28
  const uint8_t rom[7] = {0x28, 0xFF, 0x64, 0x1E, 0x0F, 0x16, 0x03};
  uint8_t c = ow::crc8(rom, 7);
  uint8_t full[8];
  std::memcpy(full, rom, 7);
  full[7] = c;
  CHECK_EQ(ow::crc8(full, 8), 0);
  const uint8_t maxim[7] = {0x02, 0x1C, 0xB8, 0x01, 0x00, 0x00, 0x00};  // AN27 example
  CHECK_EQ(ow::crc8(maxim, 7), 0xA2);
}

TEST(onewire_search_finds_all_devices) {
  SimBus bus;
  const uint64_t serials[] = {0x0000001A2B3C, 0x0000001A2B3D, 0x00F000000001, 0x7FFFFFFFFFFF, 0x000000000001};
  for (uint64_t s : serials) {
    SimDevice d;
    d.rom = makeRom(0x28, s);
    bus.devs.push_back(d);
  }
  SimDevice s20;
  s20.rom = makeRom(0x10, 0x123456);
  bus.devs.push_back(s20);
  ow::SearchResult r = ow::search(bus, 16);
  CHECK_STR(r.error, "");
  CHECK(r.presence);
  CHECK_EQ(r.roms.size(), bus.devs.size());
  for (auto& d : bus.devs) {
    bool found = false;
    for (auto& x : r.roms) found = found || x == d.rom;
    CHECK(found);
  }
  // limit respected
  CHECK_EQ(ow::search(bus, 2).roms.size(), 2u);
}

TEST(onewire_empty_shorted_and_noisy_bus) {
  SimBus empty;
  ow::SearchResult r = ow::search(empty, 16);
  CHECK(!r.presence);
  CHECK(r.roms.empty());
  CHECK_STR(r.error, "");

  SimBus shorted;
  shorted.shorted = true;
  SimDevice d;
  d.rom = makeRom(0x28, 1);
  shorted.devs.push_back(d);
  r = ow::search(shorted, 16);
  CHECK(r.roms.empty());
  CHECK(!r.error.empty());  // all-zero ROM is rejected
  uint8_t sp[9];
  CHECK(!ow::readScratchpad(shorted, d.rom, sp));

  SimBus noisy;
  noisy.devs.push_back(d);
  noisy.corruptBit = 5;  // corrupts the first ROM bit pair
  r = ow::search(noisy, 16);
  CHECK(r.roms.size() <= 1);
  if (r.roms.size() == 1) CHECK(r.roms[0] == d.rom);
}

TEST(onewire_convert_and_read_temperatures) {
  SimBus bus;
  const float temps[] = {21.5625f, -10.125f, 0.f, 55.75f};
  for (int i = 0; i < 4; ++i) {
    SimDevice d;
    d.rom = makeRom(i == 1 ? 0x22 : 0x28, 100 + i);
    d.temp = temps[i];
    d.setCrc();
    bus.devs.push_back(d);
  }
  SimDevice s20;
  s20.rom = makeRom(0x10, 7);
  s20.temp = 19.5f;
  s20.sp[0] = 0xAA;
  s20.sp[1] = 0x00;
  s20.setCrc();
  bus.devs.push_back(s20);

  // before any conversion: power-on value is recognised
  uint8_t sp[9];
  CHECK(ow::readScratchpad(bus, bus.devs[0].rom, sp));
  CHECK(ow::isPowerOnValue(0x28, sp));
  CHECK(ow::readScratchpad(bus, s20.rom, sp));
  CHECK(ow::isPowerOnValue(0x10, sp));

  CHECK(ow::startConversion(bus));
  for (size_t i = 0; i < bus.devs.size(); ++i) {
    const auto& d = bus.devs[i];
    CHECK(ow::readScratchpad(bus, d.rom, sp));
    CHECK(!ow::isPowerOnValue(d.rom[0], sp));
    float c = -99.f;
    CHECK(ow::decodeTemperature(d.rom[0], sp, c));
    float expect = d.rom[0] == 0x10 ? 19.5f : temps[i];
    CHECK_NEAR(c, expect, 0.07f);
  }
  // a corrupted scratchpad is rejected by the CRC
  bus.corruptBit = 3;
  CHECK(!ow::readScratchpad(bus, bus.devs[0].rom, sp));
}

TEST(onewire_decode_resolutions_and_negative) {
  uint8_t sp[9] = {0, 0, 0, 0, 0x7F, 0xFF, 0, 0x10, 0};
  float c = 0;
  // -0.5 C = 0xFFF8
  sp[0] = 0xF8;
  sp[1] = 0xFF;
  CHECK(ow::decodeTemperature(0x28, sp, c));
  CHECK_NEAR(c, -0.5f, 0.001f);
  // +25.0625 at 12 bit = 0x0191; at 9 bit config the LSBs are undefined
  sp[0] = 0x91;
  sp[1] = 0x01;
  CHECK(ow::decodeTemperature(0x28, sp, c));
  CHECK_NEAR(c, 25.0625f, 0.001f);
  sp[4] = 0x1F;  // 9 bit
  CHECK(ow::decodeTemperature(0x28, sp, c));
  CHECK_NEAR(c, 25.0f, 0.001f);
  // implausible values are rejected
  sp[4] = 0x7F;
  sp[0] = 0x00;
  sp[1] = 0x08;  // 128 C
  CHECK(!ow::decodeTemperature(0x28, sp, c));
  CHECK(ow::isTemperatureFamily(0x28) && ow::isTemperatureFamily(0x10) && !ow::isTemperatureFamily(0x01));
  ow::Rom r = {0x28, 0xAB, 0, 0, 0, 0, 0x01, 0xFE};
  CHECK_STR(ow::romHex(r), "28AB0000000001FE");
}
