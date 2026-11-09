#pragma once
// Decoders for Bluetooth LE thermometer advertisements (passive scanning,
// no connection, nothing is sent to the device).
//
// Supported formats:
//   * BTHome v2 (UUID 0xFCD2), plain and AES-CCM encrypted - Shelly BLU H&T,
//     Xiaomi/Qingping thermometers with pvvx or ATC firmware set to BTHome
//   * pvvx custom format (UUID 0x181A, 15 bytes) - pvvx firmware default
//   * ATC1441 format (UUID 0x181A, 13 bytes)
//   * Xiaomi MiBeacon (UUID 0xFE95), plain and v4/v5 encrypted (bindkey) -
//     LYWSD03MMC, LYWSDCGQ, MHO-C401, ... with stock firmware
//   * Govee H5072/H5075 and H5074/H5051 (manufacturer data 0xEC88)
//   * SwitchBot Meter / Meter Plus (service data 0x0D00 / 0xFD3D)
// The formats are implemented from the published specifications and the
// open-source decoders (BTHome spec, pvvx docs, Home Assistant xiaomi-ble /
// govee-ble / switchbot); verified with test vectors, not with every device.
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace hc {
namespace ble {

using Mac = std::array<uint8_t, 6>;  // display order: A4:C1:38:... = {0xA4, 0xC1, 0x38, ...}
using Key = std::array<uint8_t, 16>;

enum class Format : uint8_t { None = 0, BtHome, Pvvx, Atc, MiBeacon, Govee, SwitchBot };
const char* formatName(Format f);

struct Reading {
  Format format = Format::None;
  bool encrypted = false;
  bool needsKey = false;  // encrypted and no key known
  bool keyError = false;  // key known but the message does not authenticate
  bool unsupported = false;  // recognised but not decodable (e.g. legacy MiBeacon encryption)
  bool hasTemp = false;
  float temp = 0.f;
  bool hasHum = false;
  float hum = 0.f;
  int batteryPct = -1;
  int64_t counter = -1;  // packet/frame counter (duplicate suppression), -1 = none
  std::string name;      // local name from the advertisement, if any
  bool hasDeviceMac = false;  // the message names the device (MiBeacon): identity for keys and sensor ids
  Mac deviceMac{};
};

// Returns the key for a device or nullptr.
using KeyLookup = std::function<const Key*(const Mac&)>;

// Quick check whether an advertisement can carry thermometer data (used in
// the radio callback before anything is copied).
bool isCandidate(const uint8_t* adv, size_t len);

// Decodes one advertisement (AD structures as received). Returns true if a
// supported format was recognised (also when encrypted without key).
bool decode(const Mac& mac, const uint8_t* adv, size_t len, const KeyLookup& keys, Reading& out);

std::string macHex(const Mac& m);                        // "A4C1381A2B3C"
bool parseMac(const std::string& s, Mac& out);           // accepts "A4:C1:..", "a4c1..."
bool parseKey(const std::string& hex, Key& out);         // 32 hex digits
// NimBLE delivers addresses least significant byte first
Mac macFromLittleEndian(const uint8_t le[6]);

}  // namespace ble
}  // namespace hc
