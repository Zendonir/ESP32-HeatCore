#include "hc/rf/RfCodec.h"

#include <cmath>
#include <cstring>

namespace hc {
namespace rf {

const char* protocolName(Protocol p) {
  switch (p) {
    case Protocol::Ev1527: return "ev1527";
    case Protocol::Pt2262: return "pt2262";
    default: return "unknown";
  }
}

Protocol protocolFromName(const char* s) {
  if (!s) return Protocol::Unknown;
  if (std::strcmp(s, "ev1527") == 0) return Protocol::Ev1527;
  if (std::strcmp(s, "pt2262") == 0) return Protocol::Pt2262;
  return Protocol::Unknown;
}

std::string triStateString(uint32_t code, uint8_t bits) {
  if (bits == 0 || bits % 2) return std::string();
  std::string s;
  for (int i = bits - 2; i >= 0; i -= 2) {
    uint32_t pair = (code >> i) & 0x3;
    if (pair == 0) s += '0';
    else if (pair == 3) s += '1';
    else if (pair == 1) s += 'F';
    else return std::string();  // "10" is not a valid PT2262 symbol
  }
  return s;
}

// ---------------------------------------------------------------- Segmenter

void Segmenter::feed(const Pulse* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    Pulse cur = p[i];
    if (cur.us < cfg_.glitchUs) {
      // A short spike interrupts the pending pulse: fold it (and the following
      // continuation of the pending level) into the pending pulse.
      ++glitches_;
      if (havePending_) {
        uint32_t sum = static_cast<uint32_t>(pending_.us) + cur.us;
        pending_.us = sum > 65535 ? 65535 : static_cast<uint16_t>(sum);
        mergeNext_ = true;
      }
      continue;
    }
    if (havePending_ && mergeNext_ && cur.level == pending_.level) {
      uint32_t sum = static_cast<uint32_t>(pending_.us) + cur.us;
      pending_.us = sum > 65535 ? 65535 : static_cast<uint16_t>(sum);
      mergeNext_ = false;
      continue;
    }
    mergeNext_ = false;
    if (havePending_) push(pending_);
    pending_ = cur;
    havePending_ = true;
  }
}

void Segmenter::flushIdle() {
  if (havePending_) {
    push(pending_);
    havePending_ = false;
    mergeNext_ = false;
  }
  if (!buf_.empty()) {
    emit(65535);
    buf_.clear();
  }
  leadGap_ = 0;
}

void Segmenter::push(Pulse p) {
  if (p.level == 0 && p.us >= cfg_.boundaryUs) {
    emit(p.us);
    buf_.clear();
    leadGap_ = p.us;
    return;
  }
  if (buf_.empty() && p.level == 0) return;  // segments always start with a high pulse
  if (buf_.size() >= kMaxSegment) {
    ++overflows_;
    buf_.clear();
    leadGap_ = 0;
    return;
  }
  buf_.push_back(p);
}

void Segmenter::emit(uint32_t tailGapUs) {
  if (buf_.empty()) return;
  ++segments_;
  if (fn_) fn_(ctx_, buf_.data(), buf_.size(), leadGap_, tailGapUs);
}

// ------------------------------------------------------------ P1 decoding

namespace {

struct BitResult {
  bool ok;
  int bit;
  float err;  // normalised 0..1 (1 == at tolerance limit)
};

BitResult classifyPair(const CodecConfig& cfg, float T, uint16_t h, uint16_t l) {
  float period = static_cast<float>(h) + static_cast<float>(l);
  float periodErr = std::fabs(period - 4.f * T) / (4.f * T);
  float frac = static_cast<float>(h) / period;
  int bit = frac > 0.5f ? 1 : 0;
  float expected = bit ? 0.75f : 0.25f;
  // frac may deviate by up to 0.18 (receiver AGC stretches high pulses).
  float fracErr = std::fabs(frac - expected) / 0.18f;
  float pErr = periodErr / cfg.maxTimingError;
  float err = fracErr > pErr ? fracErr : pErr;
  return {err <= 1.f, bit, err};
}

BitResult classifyLoneHigh(const CodecConfig& cfg, float T, uint16_t h) {
  float e0 = std::fabs(h - T) / T;
  float e1 = std::fabs(h - 3.f * T) / (3.f * T);
  int bit = e1 < e0 ? 1 : 0;
  float err = (bit ? e1 : e0) / cfg.maxTimingError;
  return {err <= 1.f, bit, err};
}

bool tryDecode(const CodecConfig& cfg, const Pulse* p, size_t n, float T, size_t pairs, bool trailingSync,
               bool loneLast, DecodedFrame& out) {
  uint32_t code = 0;
  float errSum = 0.f;
  size_t bits = 0;
  for (size_t i = 0; i < pairs; ++i) {
    const Pulse& h = p[2 * i];
    const Pulse& l = p[2 * i + 1];
    if (h.level != 1 || l.level != 0) return false;
    BitResult r = classifyPair(cfg, T, h.us, l.us);
    if (!r.ok) return false;
    code = (code << 1) | static_cast<uint32_t>(r.bit);
    errSum += r.err;
    ++bits;
  }
  if (loneLast) {
    const Pulse& h = p[n - 1];
    if (h.level != 1) return false;
    BitResult r = classifyLoneHigh(cfg, T, h.us);
    if (!r.ok) return false;
    code = (code << 1) | static_cast<uint32_t>(r.bit);
    errSum += r.err;
    ++bits;
  }
  if (trailingSync) {
    const Pulse& s = p[n - 1];
    if (s.level != 1) return false;
    float e = std::fabs(s.us - T) / T;
    if (e > 1.0f) return false;  // lenient: AGC can stretch the sync high
  }
  if (bits < 8 || bits > 32) return false;
  out.code = code;
  out.bits = static_cast<uint8_t>(bits);
  float meanErr = errSum / static_cast<float>(bits);
  float q = 100.f * (1.f - meanErr);
  out.quality = static_cast<uint8_t>(q < 0.f ? 0.f : (q > 100.f ? 100.f : q));
  return true;
}

}  // namespace

bool decodeP1Segment(const CodecConfig& cfg, const Pulse* p, size_t n, uint32_t leadGapUs, DecodedFrame& out) {
  if (n < 15 || leadGapUs == 0 || leadGapUs >= 65535) return false;
  float T = static_cast<float>(leadGapUs) / 31.f;
  if (T < cfg.minPulseUs || T > cfg.maxPulseUs) return false;
  DecodedFrame f;
  bool ok = false;
  if (n % 2 == 1) {
    // Either [pairs..., trailing sync high] or [pairs..., last bit high only].
    size_t pairsA = (n - 1) / 2;
    if (pairsA == 24) ok = tryDecode(cfg, p, n, T, pairsA, true, false, f);
    if (!ok && pairsA + 1 == 24) ok = tryDecode(cfg, p, n, T, pairsA, false, true, f);
    if (!ok) ok = tryDecode(cfg, p, n, T, pairsA, true, false, f);
  } else {
    ok = tryDecode(cfg, p, n, T, n / 2, false, false, f);
  }
  if (!ok) return false;
  // Refine T from the complete frame: every bit is 4T, the sync is 32T.
  uint32_t total = leadGapUs;
  size_t dataPulses = static_cast<size_t>(f.bits) * 2;
  for (size_t i = 0; i < n && i < dataPulses; ++i) total += p[i].us;
  float refined = static_cast<float>(total) / (4.f * f.bits + 31.f);
  f.pulseUs = static_cast<uint16_t>(refined + 0.5f);
  f.pt2262Valid = f.bits == 24 && !triStateString(f.code, f.bits).empty();
  f.protocol = f.bits == 24 ? Protocol::Ev1527 : Protocol::Unknown;
  out = f;
  return true;
}

// -------------------------------------------------------------- encoding

void encodeP1Frame(uint32_t code, uint8_t bits, uint16_t T, std::vector<Pulse>& out) {
  auto mul = [](uint16_t t, uint32_t k) -> uint16_t {
    uint32_t v = static_cast<uint32_t>(t) * k;
    return v > 65535 ? 65535 : static_cast<uint16_t>(v);
  };
  out.push_back({T, 1});
  out.push_back({mul(T, 31), 0});
  for (int i = bits - 1; i >= 0; --i) {
    bool one = (code >> i) & 1u;
    out.push_back({one ? mul(T, 3) : T, 1});
    out.push_back({one ? T : mul(T, 3), 0});
  }
}

std::vector<Pulse> encodeP1Transmission(uint32_t code, uint8_t bits, uint16_t T, uint8_t repeats) {
  std::vector<Pulse> out;
  if (repeats == 0) repeats = 1;
  out.reserve(static_cast<size_t>(repeats) * (2 + 2 * bits) + 2);
  for (uint8_t r = 0; r < repeats; ++r) encodeP1Frame(code, bits, T, out);
  // Terminating sync so that the receiver can delimit the last frame.
  uint32_t gap = static_cast<uint32_t>(T) * 31;
  out.push_back({T, 1});
  out.push_back({static_cast<uint16_t>(gap > 65535 ? 65535 : gap), 0});
  return out;
}

uint32_t transmissionDurationUs(const std::vector<Pulse>& pulses) {
  uint32_t t = 0;
  for (const auto& p : pulses) t += p.us;
  return t;
}

CodeSplit splitCode(Protocol p, uint32_t code, uint8_t bits) {
  CodeSplit s;
  if (p == Protocol::Ev1527 && bits == 24) {
    s.address = code >> 4;
    s.data = code & 0xF;
    s.addressBits = 20;
  } else if (p == Protocol::Pt2262 && bits == 24) {
    s.address = code >> 8;
    s.data = code & 0xFF;
    s.addressBits = 16;
  } else {
    s.address = code;
    s.data = 0;
    s.addressBits = bits;
  }
  return s;
}

}  // namespace rf
}  // namespace hc
