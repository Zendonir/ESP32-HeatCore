#pragma once
// AES-128 (encryption direction only) and AES-CCM decryption for encrypted
// Bluetooth thermometer advertisements (BTHome v2, Xiaomi MiBeacon v4/v5).
// Portable C++ so it runs and is tested on the host as well.
#include <cstddef>
#include <cstdint>

namespace hc {
namespace ble {

class Aes128 {
 public:
  explicit Aes128(const uint8_t key[16]);
  void encryptBlock(const uint8_t in[16], uint8_t out[16]) const;

 private:
  uint8_t rk_[176];
};

// CCM (RFC 3610) decryption and verification. nonceLen 7..13, tagLen 4..16
// (even). Returns false if the tag does not match; `out` (ctLen bytes) is
// only meaningful on success.
bool ccmDecrypt(const uint8_t key[16], const uint8_t* nonce, size_t nonceLen, const uint8_t* aad, size_t aadLen,
                const uint8_t* ct, size_t ctLen, const uint8_t* tag, size_t tagLen, uint8_t* out);

}  // namespace ble
}  // namespace hc
