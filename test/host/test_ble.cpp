// Bluetooth LE thermometer advertisements: crypto and decoders.
#include <cstring>
#include <vector>

#include "TestFramework.h"
#include "hc/ble/BleCrypto.h"
#include "hc/ble/BleDecoders.h"

using namespace hc;
using namespace hc::ble;

namespace {

Mac mac(const char* s) {
  Mac m{};
  parseMac(s, m);
  return m;
}

const Key* noKey(const Mac&) { return nullptr; }

// AD structure helpers
std::vector<uint8_t> serviceData(uint16_t uuid, const std::vector<uint8_t>& d) {
  std::vector<uint8_t> a = {0x02, 0x01, 0x06, static_cast<uint8_t>(d.size() + 3), 0x16,
                            static_cast<uint8_t>(uuid & 0xFF), static_cast<uint8_t>(uuid >> 8)};
  a.insert(a.end(), d.begin(), d.end());
  return a;
}

std::vector<uint8_t> mfrData(uint16_t company, const std::vector<uint8_t>& d) {
  std::vector<uint8_t> a = {static_cast<uint8_t>(d.size() + 3), 0xFF, static_cast<uint8_t>(company & 0xFF),
                            static_cast<uint8_t>(company >> 8)};
  a.insert(a.end(), d.begin(), d.end());
  return a;
}

bool dec(const char* m, const std::vector<uint8_t>& adv, Reading& r, const KeyLookup& keys = noKey) {
  return decode(mac(m), adv.data(), adv.size(), keys, r);
}

}  // namespace

TEST(ble_aes_fips197_and_ccm_rfc3610) {
  uint8_t key[16], pt[16], out[16];
  for (int i = 0; i < 16; ++i) key[i] = static_cast<uint8_t>(i), pt[i] = static_cast<uint8_t>(i * 0x11);
  Aes128(key).encryptBlock(pt, out);
  const uint8_t expect[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                              0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
  CHECK(std::memcmp(out, expect, 16) == 0);

  // RFC 3610 packet vector #1: 8 byte header (AAD), 23 byte payload, 8 byte tag
  uint8_t k[16];
  for (int i = 0; i < 16; ++i) k[i] = static_cast<uint8_t>(0xC0 + i);
  const uint8_t nonce[13] = {0x00, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};
  uint8_t aad[8];
  for (int i = 0; i < 8; ++i) aad[i] = static_cast<uint8_t>(i);
  const uint8_t ct[23] = {0x58, 0x8c, 0x97, 0x9a, 0x61, 0xc6, 0x63, 0xd2, 0xf0, 0x66, 0xd0, 0xc2,
                          0xc0, 0xf9, 0x89, 0x80, 0x6d, 0x5f, 0x6b, 0x61, 0xda, 0xc3, 0x84};
  uint8_t tag[8] = {0x17, 0xe8, 0xd1, 0x2c, 0xfd, 0xf9, 0x26, 0xe0};
  uint8_t plain[23];
  CHECK(ccmDecrypt(k, nonce, 13, aad, 8, ct, 23, tag, 8, plain));
  for (int i = 0; i < 23; ++i) CHECK_EQ(plain[i], i + 8);
  tag[7] ^= 1;
  CHECK(!ccmDecrypt(k, nonce, 13, aad, 8, ct, 23, tag, 8, plain));
}

TEST(ble_bthome_plain) {
  // packet id 9, battery 93 %, temperature 21.37 C, humidity 45.50 %, unknown trailing object stops parsing
  Reading r;
  CHECK(dec("A4:C1:38:00:00:01",
            serviceData(0xFCD2, {0x40, 0x00, 0x09, 0x01, 93, 0x02, 0x59, 0x08, 0x03, 0xC6, 0x11, 0x53, 0x03}), r));
  CHECK(r.format == Format::BtHome);
  CHECK(!r.encrypted);
  CHECK(r.hasTemp && r.hasHum);
  CHECK_NEAR(r.temp, 21.37f, 0.001f);
  CHECK_NEAR(r.hum, 45.5f, 0.001f);
  CHECK_EQ(r.batteryPct, 93);
  CHECK_EQ(r.counter, 9);
  // 0.1 K temperature object and u8 humidity (Shelly BLU H&T style), negative value
  CHECK(dec("A4:C1:38:00:00:01", serviceData(0xFCD2, {0x44, 0x45, 0x83, 0xFF, 0x2E, 60}), r));
  CHECK_NEAR(r.temp, -12.5f, 0.001f);
  CHECK_NEAR(r.hum, 60.f, 0.001f);
  // BTHome v1 / other versions: recognised but not decoded
  CHECK(dec("A4:C1:38:00:00:01", serviceData(0xFCD2, {0x20, 0x02, 0x00, 0x00}), r));
  CHECK(r.unsupported && !r.hasTemp);
}

TEST(ble_bthome_encrypted) {
  // generated with Python cryptography AESCCM (key, MAC from the BTHome docs example)
  const std::vector<uint8_t> adv = {0x02, 0x01, 0x06, 0x14, 0x16, 0xD2, 0xFC, 0x41, 0xE7, 0xD8, 0xF8, 0x93,
                                    0x21, 0x3B, 0x6C, 0x05, 0x33, 0x22, 0x11, 0x00, 0x6C, 0xA4, 0x67, 0x96};
  Key key{};
  CHECK(parseKey("231d39c1d7cc1ab1aee224cd096db932", key));
  Mac m = mac("54:48:E6:8F:80:A5");
  KeyLookup keys = [&](const Mac& x) -> const Key* { return x == m ? &key : nullptr; };
  Reading r;
  CHECK(decode(m, adv.data(), adv.size(), noKey, r));
  CHECK(r.encrypted && r.needsKey && !r.hasTemp);
  CHECK(decode(m, adv.data(), adv.size(), keys, r));
  CHECK(!r.needsKey && !r.keyError);
  CHECK_NEAR(r.temp, 21.37f, 0.001f);
  CHECK_NEAR(r.hum, 45.5f, 0.001f);
  CHECK_EQ(r.batteryPct, 87);
  CHECK_EQ(r.counter, 0x00112233);
  // wrong key / other device: authentication fails
  Key wrong = key;
  wrong[0] ^= 0x80;
  KeyLookup bad = [&](const Mac&) -> const Key* { return &wrong; };
  CHECK(decode(m, adv.data(), adv.size(), bad, r));
  CHECK(r.keyError && !r.hasTemp);
  CHECK(decode(mac("54:48:E6:8F:80:A6"), adv.data(), adv.size(), [&](const Mac&) -> const Key* { return &key; }, r));
  CHECK(r.keyError);  // the MAC is part of the nonce
}

TEST(ble_pvvx_and_atc) {
  Reading r;
  // pvvx: MAC le, 23.45 C, 51.20 %, 2950 mV, 76 %, counter 0x21, flags
  CHECK(dec("A4:C1:38:11:22:33",
            serviceData(0x181A, {0x33, 0x22, 0x11, 0x38, 0xC1, 0xA4, 0x29, 0x09, 0x00, 0x14, 0x86, 0x0B, 76, 0x21, 0x04}),
            r));
  CHECK(r.format == Format::Pvvx);
  CHECK_NEAR(r.temp, 23.45f, 0.001f);
  CHECK_NEAR(r.hum, 51.2f, 0.001f);
  CHECK_EQ(r.batteryPct, 76);
  // ATC1441: MAC be, -3.2 C (0xFFE0 be), 64 %, 88 %, 3000 mV, counter
  CHECK(dec("A4:C1:38:11:22:33",
            serviceData(0x181A, {0xA4, 0xC1, 0x38, 0x11, 0x22, 0x33, 0xFF, 0xE0, 64, 88, 0x0B, 0xB8, 0x07}), r));
  CHECK(r.format == Format::Atc);
  CHECK_NEAR(r.temp, -3.2f, 0.001f);
  CHECK_NEAR(r.hum, 64.f, 0.001f);
  CHECK_EQ(r.batteryPct, 88);
  // other length on 0x181A: not a thermometer format
  CHECK(!dec("A4:C1:38:11:22:33", serviceData(0x181A, {1, 2, 3}), r));
}

TEST(ble_mibeacon_plain_and_encrypted) {
  Reading r;
  // plain v3 frame with MAC: temperature + humidity object 0x100D (22.1 C, 48.3 %)
  CHECK(dec("4C:65:A8:DD:EE:FF",
            serviceData(0xFE95, {0x50, 0x30, 0xAA, 0x01, 0x10, 0xFF, 0xEE, 0xDD, 0xA8, 0x65, 0x4C, 0x0D, 0x10, 0x04,
                                 0xDD, 0x00, 0xE3, 0x01}),
            r));
  CHECK(r.format == Format::MiBeacon && !r.encrypted);
  CHECK_NEAR(r.temp, 22.1f, 0.001f);
  CHECK_NEAR(r.hum, 48.3f, 0.001f);
  // battery only
  CHECK(dec("4C:65:A8:DD:EE:FF",
            serviceData(0xFE95, {0x50, 0x30, 0xAA, 0x01, 0x11, 0xFF, 0xEE, 0xDD, 0xA8, 0x65, 0x4C, 0x0A, 0x10, 0x01, 61}),
            r));
  CHECK(!r.hasTemp);
  CHECK_EQ(r.batteryPct, 61);

  // v5 encrypted (LYWSD03MMC stock firmware), generated with Python AESCCM: 23.4 C
  const std::vector<uint8_t> adv = {0x02, 0x01, 0x06, 0x1A, 0x16, 0x95, 0xFE, 0x58, 0x50, 0x5B, 0x05,
                                    0x37, 0x3C, 0x2B, 0x1A, 0x38, 0xC1, 0xA4, 0x13, 0xED, 0x9E, 0x75,
                                    0xD6, 0x01, 0x02, 0x03, 0x4C, 0x6A, 0x50, 0x1D};
  Key key{};
  CHECK(parseKey("e9efaa6873f9f9c87a5e75a5f814801c", key));
  Mac m = mac("A4:C1:38:1A:2B:3C");
  CHECK(decode(m, adv.data(), adv.size(), noKey, r));
  CHECK(r.encrypted && r.needsKey);
  CHECK(decode(m, adv.data(), adv.size(), [&](const Mac& x) -> const Key* { return x == m ? &key : nullptr; }, r));
  CHECK(!r.keyError);
  CHECK(r.hasTemp && !r.hasHum);
  CHECK_NEAR(r.temp, 23.4f, 0.001f);
  // the MAC inside the frame counts, not a (random) advertiser address
  CHECK(decode(mac("11:22:33:44:55:66"), adv.data(), adv.size(),
               [&](const Mac& x) -> const Key* { return x == m ? &key : nullptr; }, r));
  CHECK_NEAR(r.temp, 23.4f, 0.001f);
  CHECK(r.hasDeviceMac && r.deviceMac == m);
  // legacy encryption (version 3) is reported as unsupported
  CHECK(dec("4C:65:A8:DD:EE:FF", serviceData(0xFE95, {0x58, 0x30, 0xAA, 0x01, 0x10, 1, 2, 3, 4, 5, 6, 7, 8}), r));
  CHECK(r.unsupported);
}

TEST(ble_govee_and_switchbot) {
  Reading r;
  // H5075: 0x034C5B = 216155 -> 21.6 C, 15.5 %, battery 100
  std::vector<uint8_t> a = mfrData(0xEC88, {0x00, 0x03, 0x4C, 0x5B, 0x64, 0x00});
  CHECK(dec("A4:C1:38:AA:BB:CC", a, r));
  CHECK(r.format == Format::Govee);
  CHECK_NEAR(r.temp, 21.6f, 0.001f);
  CHECK_NEAR(r.hum, 15.5f, 0.001f);
  CHECK_EQ(r.batteryPct, 100);
  // negative: bit 23 set, 51015 -> -5.1 C, 1.5 %
  CHECK(dec("A4:C1:38:AA:BB:CC", mfrData(0xEC88, {0x00, 0x80, 0xC7, 0x47, 0x40, 0x00}), r));
  CHECK_NEAR(r.temp, -5.1f, 0.001f);
  CHECK_NEAR(r.hum, 1.5f, 0.001f);
  // H5074: 22.50 C, 40.00 %, 77 %
  CHECK(dec("A4:C1:38:AA:BB:CC", mfrData(0xEC88, {0x00, 0xCA, 0x08, 0xA0, 0x0F, 77, 0x02}), r));
  CHECK_NEAR(r.temp, 22.5f, 0.001f);
  CHECK_NEAR(r.hum, 40.f, 0.001f);
  CHECK_EQ(r.batteryPct, 77);
  // SwitchBot Meter: 'T', battery 90, 23.5 C, 47 %; and -4.2 C
  CHECK(dec("C0:00:00:00:00:01", serviceData(0x0D00, {0x54, 0x00, 90, 0x05, 0x97, 47}), r));
  CHECK(r.format == Format::SwitchBot);
  CHECK_NEAR(r.temp, 23.5f, 0.001f);
  CHECK_NEAR(r.hum, 47.f, 0.001f);
  CHECK_EQ(r.batteryPct, 90);
  CHECK(dec("C0:00:00:00:00:01", serviceData(0xFD3D, {0x69, 0x00, 90, 0x02, 0x04, 47}), r));
  CHECK_NEAR(r.temp, -4.2f, 0.001f);
  CHECK(!dec("C0:00:00:00:00:01", serviceData(0xFD3D, {0x48, 0x00, 90, 0x02, 0x04, 47}), r));  // bot, no meter
}

TEST(ble_filter_name_and_garbage) {
  // name + pvvx data: candidate, name captured
  std::vector<uint8_t> a = {0x0B, 0x09, 'A', 'T', 'C', '_', '1', 'A', '2', 'B', '3', 'C'};
  std::vector<uint8_t> s = serviceData(0x181A, {0x33, 0x22, 0x11, 0x38, 0xC1, 0xA4, 0x29, 0x09, 0x00, 0x14, 0x86,
                                                0x0B, 76, 0x21, 0x04});
  a.insert(a.end(), s.begin(), s.end());
  CHECK(isCandidate(a.data(), a.size()));
  Reading r;
  CHECK(dec("A4:C1:38:11:22:33", a, r));
  CHECK_STR(r.name, "ATC_1A2B3C");
  // phones, beacons etc. are no candidates
  std::vector<uint8_t> apple = mfrData(0x004C, {0x10, 0x05, 0x01});
  CHECK(!isCandidate(apple.data(), apple.size()));
  CHECK(!dec("11:22:33:44:55:66", apple, r));
  // truncated / malformed AD structures never read past the end
  for (size_t n = 0; n < s.size(); ++n) {
    isCandidate(s.data(), n);
    dec("A4:C1:38:11:22:33", std::vector<uint8_t>(s.begin(), s.begin() + static_cast<long>(n)), r);
  }
  std::vector<uint8_t> bad = {0x1F, 0x16, 0xD2, 0xFC, 0x41};
  CHECK(!dec("A4:C1:38:11:22:33", bad, r));
  // implausible values are dropped
  CHECK(dec("A4:C1:38:11:22:33", serviceData(0xFCD2, {0x40, 0x02, 0xFF, 0x7F}), r));
  CHECK(!r.hasTemp);
  // helpers
  Mac m{};
  CHECK(parseMac("a4-c1-38-1a-2b-3c", m));
  CHECK_STR(macHex(m), "A4C1381A2B3C");
  CHECK(!parseMac("A4C1381A2B", m));
  Key k{};
  CHECK(!parseKey("xyz", k));
  const uint8_t le[6] = {0x3C, 0x2B, 0x1A, 0x38, 0xC1, 0xA4};
  CHECK(macFromLittleEndian(le) == mac("A4C1381A2B3C"));
}
