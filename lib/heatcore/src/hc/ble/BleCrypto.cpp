#include "hc/ble/BleCrypto.h"

#include <cstring>

namespace hc {
namespace ble {

namespace {

const uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9,
    0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f,
    0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07,
    0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3,
    0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58,
    0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3,
    0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f,
    0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
    0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac,
    0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a,
    0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70,
    0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42,
    0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

uint8_t xtime(uint8_t x) { return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00)); }

}  // namespace

Aes128::Aes128(const uint8_t key[16]) {
  std::memcpy(rk_, key, 16);
  uint8_t rcon = 1;
  for (int i = 16; i < 176; i += 4) {
    uint8_t t[4] = {rk_[i - 4], rk_[i - 3], rk_[i - 2], rk_[i - 1]};
    if (i % 16 == 0) {
      uint8_t f = t[0];
      t[0] = static_cast<uint8_t>(kSbox[t[1]] ^ rcon);
      t[1] = kSbox[t[2]];
      t[2] = kSbox[t[3]];
      t[3] = kSbox[f];
      rcon = xtime(rcon);
    }
    for (int k = 0; k < 4; ++k) rk_[i + k] = static_cast<uint8_t>(rk_[i - 16 + k] ^ t[k]);
  }
}

void Aes128::encryptBlock(const uint8_t in[16], uint8_t out[16]) const {
  uint8_t s[16];
  for (int i = 0; i < 16; ++i) s[i] = static_cast<uint8_t>(in[i] ^ rk_[i]);
  for (int round = 1; round <= 10; ++round) {
    for (int i = 0; i < 16; ++i) s[i] = kSbox[s[i]];
    // shift rows (column-major state: index = col * 4 + row)
    uint8_t t = s[1];
    s[1] = s[5], s[5] = s[9], s[9] = s[13], s[13] = t;
    t = s[2], s[2] = s[10], s[10] = t;
    t = s[6], s[6] = s[14], s[14] = t;
    t = s[15], s[15] = s[11], s[11] = s[7], s[7] = s[3], s[3] = t;
    if (round < 10) {
      for (int c = 0; c < 4; ++c) {
        uint8_t* col = s + 4 * c;
        uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        uint8_t all = static_cast<uint8_t>(a0 ^ a1 ^ a2 ^ a3);
        col[0] = static_cast<uint8_t>(a0 ^ all ^ xtime(static_cast<uint8_t>(a0 ^ a1)));
        col[1] = static_cast<uint8_t>(a1 ^ all ^ xtime(static_cast<uint8_t>(a1 ^ a2)));
        col[2] = static_cast<uint8_t>(a2 ^ all ^ xtime(static_cast<uint8_t>(a2 ^ a3)));
        col[3] = static_cast<uint8_t>(a3 ^ all ^ xtime(static_cast<uint8_t>(a3 ^ a0)));
      }
    }
    for (int i = 0; i < 16; ++i) s[i] = static_cast<uint8_t>(s[i] ^ rk_[16 * round + i]);
  }
  std::memcpy(out, s, 16);
}

bool ccmDecrypt(const uint8_t key[16], const uint8_t* nonce, size_t nonceLen, const uint8_t* aad, size_t aadLen,
                const uint8_t* ct, size_t ctLen, const uint8_t* tag, size_t tagLen, uint8_t* out) {
  if (nonceLen < 7 || nonceLen > 13 || tagLen < 4 || tagLen > 16 || (tagLen & 1) || aadLen > 0xFEFF) return false;
  const size_t L = 15 - nonceLen;
  if (L < 8 && ctLen >= (size_t{1} << (8 * L))) return false;
  Aes128 aes(key);
  uint8_t ctr[16], s[16];
  // counter blocks A_i = flags(L-1) | nonce | i
  auto counter = [&](uint32_t i) {
    std::memset(ctr, 0, 16);
    ctr[0] = static_cast<uint8_t>(L - 1);
    std::memcpy(ctr + 1, nonce, nonceLen);
    for (size_t k = 0; k < L && k < 4; ++k) ctr[15 - k] = static_cast<uint8_t>(i >> (8 * k));
    aes.encryptBlock(ctr, s);
  };
  // decrypt
  for (size_t off = 0, i = 1; off < ctLen; off += 16, ++i) {
    counter(static_cast<uint32_t>(i));
    for (size_t k = 0; k < 16 && off + k < ctLen; ++k) out[off + k] = static_cast<uint8_t>(ct[off + k] ^ s[k]);
  }
  // CBC-MAC over B0, the associated data and the plaintext
  uint8_t x[16];
  uint8_t b[16] = {};
  b[0] = static_cast<uint8_t>((aadLen ? 0x40 : 0) | (((tagLen - 2) / 2) << 3) | (L - 1));
  std::memcpy(b + 1, nonce, nonceLen);
  for (size_t k = 0; k < L && k < 8; ++k) b[15 - k] = static_cast<uint8_t>(static_cast<uint64_t>(ctLen) >> (8 * k));
  aes.encryptBlock(b, x);
  auto absorb = [&](const uint8_t* d, size_t n, size_t prefix) {
    // `prefix` bytes of the first block are already filled in b
    size_t pos = prefix;
    for (size_t k = 0; k < n; ++k) {
      b[pos++] = d[k];
      if (pos == 16) {
        for (int j = 0; j < 16; ++j) b[j] ^= x[j];
        aes.encryptBlock(b, x);
        pos = 0;
      }
    }
    if (pos) {
      for (size_t j = pos; j < 16; ++j) b[j] = 0;
      for (int j = 0; j < 16; ++j) b[j] ^= x[j];
      aes.encryptBlock(b, x);
    }
  };
  if (aadLen) {
    b[0] = static_cast<uint8_t>(aadLen >> 8);
    b[1] = static_cast<uint8_t>(aadLen);
    absorb(aad, aadLen, 2);
  }
  if (ctLen) absorb(out, ctLen, 0);
  counter(0);
  uint8_t diff = 0;
  for (size_t k = 0; k < tagLen; ++k) diff |= static_cast<uint8_t>((x[k] ^ s[k]) ^ tag[k]);
  return diff == 0;
}

}  // namespace ble
}  // namespace hc
