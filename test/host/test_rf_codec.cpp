#include <random>

#include "TestFramework.h"
#include "hc/rf/RfCodec.h"

using namespace hc::rf;

namespace {

struct Collector {
  CodecConfig cfg;
  std::vector<DecodedFrame> frames;
  uint32_t segments = 0;
  static void onSegment(void* ctx, const Pulse* p, size_t n, uint32_t lead, uint32_t tail) {
    auto* self = static_cast<Collector*>(ctx);
    ++self->segments;
    DecodedFrame f;
    if (decodeP1Segment(self->cfg, p, n, lead, f)) self->frames.push_back(f);
  }
};

// Simulates a superregenerative receiver: jitter, stretched highs, glitches.
std::vector<Pulse> distort(const std::vector<Pulse>& in, std::mt19937& rng, float jitter, int highStretchUs,
                           int glitchEvery) {
  std::normal_distribution<float> n(0.f, jitter);
  std::vector<Pulse> out;
  int k = 0;
  for (auto p : in) {
    float v = p.us * (1.f + n(rng));
    if (p.level) v += highStretchUs;
    else v -= highStretchUs;
    if (v < 30) v = 30;
    if (v > 65535) v = 65535;
    if (glitchEvery && ++k % glitchEvery == 0 && v > 200) {
      // split pulse with a 20 us spike of the opposite level
      uint16_t a = static_cast<uint16_t>(v / 2);
      out.push_back({a, p.level});
      out.push_back({20, static_cast<uint8_t>(!p.level)});
      out.push_back({static_cast<uint16_t>(v - a - 20), p.level});
      continue;
    }
    out.push_back({static_cast<uint16_t>(v), p.level});
  }
  return out;
}

std::vector<Pulse> noise(std::mt19937& rng, size_t count) {
  std::uniform_int_distribution<int> d(40, 3000);
  std::vector<Pulse> out;
  uint8_t level = 1;
  for (size_t i = 0; i < count; ++i) {
    out.push_back({static_cast<uint16_t>(d(rng)), level});
    level = !level;
  }
  return out;
}

}  // namespace

TEST(rf_codec_roundtrip_clean) {
  Collector c;
  Segmenter seg(c.cfg);
  seg.setConsumer(&Collector::onSegment, &c);
  auto tx = encodeP1Transmission(0xA5C3E1, 24, 350, 5);
  seg.feed(tx.data(), tx.size());
  seg.flushIdle();
  CHECK_EQ(c.frames.size(), 5u);
  for (auto& f : c.frames) {
    CHECK_EQ(f.code, 0xA5C3E1u);
    CHECK_EQ(f.bits, 24);
    CHECK(f.protocol == Protocol::Ev1527);
    CHECK_NEAR(f.pulseUs, 350, 5);
    CHECK(f.quality > 90);
  }
}

TEST(rf_codec_variable_pulse_length) {
  for (uint16_t T : {120, 200, 330, 450, 600, 700}) {
    Collector c;
    Segmenter seg(c.cfg);
    seg.setConsumer(&Collector::onSegment, &c);
    auto tx = encodeP1Transmission(0x123456, 24, T, 3);
    seg.feed(tx.data(), tx.size());
    seg.flushIdle();
    CHECK_EQ(c.frames.size(), 3u);
    if (!c.frames.empty()) {
      CHECK_EQ(c.frames[0].code, 0x123456u);
      CHECK_NEAR(c.frames[0].pulseUs, T, T * 0.03 + 2);
    }
  }
}

TEST(rf_codec_distorted_receiver) {
  std::mt19937 rng(42);
  Collector c;
  Segmenter seg(c.cfg);
  seg.setConsumer(&Collector::onSegment, &c);
  auto lead = noise(rng, 300);
  seg.feed(lead.data(), lead.size());
  auto tx = distort(encodeP1Transmission(0x0F0F0F, 24, 320, 8), rng, 0.08f, 70, 37);
  seg.feed(tx.data(), tx.size());
  auto tail = noise(rng, 300);
  seg.feed(tail.data(), tail.size());
  seg.flushIdle();
  size_t good = 0;
  for (auto& f : c.frames)
    if (f.code == 0x0F0F0F && f.bits == 24) ++good;
  // At least 6 of 8 frames must survive jitter, AGC stretch and glitches.
  CHECK(good >= 6);
  CHECK(seg.glitches() > 0);
}

TEST(rf_codec_noise_produces_no_24bit_frames) {
  std::mt19937 rng(7);
  Collector c;
  Segmenter seg(c.cfg);
  seg.setConsumer(&Collector::onSegment, &c);
  // Long noise including occasional sync-like gaps.
  for (int round = 0; round < 200; ++round) {
    auto n = noise(rng, 1000);
    seg.feed(n.data(), n.size());
    Pulse gap{static_cast<uint16_t>(9000 + round * 13), 0};
    seg.feed(&gap, 1);
  }
  seg.flushIdle();
  size_t bits24 = 0;
  for (auto& f : c.frames)
    if (f.bits == 24) ++bits24;
  CHECK_EQ(bits24, 0u);
  CHECK(c.segments > 100);
}

TEST(rf_codec_rejects_wrong_timing) {
  CodecConfig cfg;
  auto tx = encodeP1Transmission(0x55AA55, 24, 350, 1);
  // tx = [H][Lsync][48 data][H][Lsync]; take data + trailing sync high.
  std::vector<Pulse> seg(tx.begin() + 2, tx.end() - 1);
  DecodedFrame f;
  CHECK(decodeP1Segment(cfg, seg.data(), seg.size(), 350 * 31, f));
  CHECK_EQ(f.code, 0x55AA55u);
  // Wrong sync gap -> T inconsistent with data
  CHECK(!decodeP1Segment(cfg, seg.data(), seg.size(), 150 * 31, f));
  // Corrupt one bit beyond tolerance
  seg[10].us = 50;
  seg[11].us = 3000;
  CHECK(!decodeP1Segment(cfg, seg.data(), seg.size(), 350 * 31, f));
}

TEST(rf_codec_last_frame_without_trailing_sync) {
  CodecConfig cfg;
  std::vector<Pulse> frame;
  encodeP1Frame(0xABCDEF, 24, 400, frame);
  // drop the last low: the final gap becomes the segment boundary
  std::vector<Pulse> seg(frame.begin() + 2, frame.end() - 1);
  DecodedFrame f;
  CHECK(decodeP1Segment(cfg, seg.data(), seg.size(), 400 * 31, f));
  CHECK_EQ(f.code, 0xABCDEFu);
}

TEST(rf_codec_tristate_and_split) {
  // PT2262 symbols: 0=00, 1=11, F=01 -> "0F1F00001F11"
  uint32_t code = 0;
  const char* sym = "0F1F00001F11";
  for (const char* s = sym; *s; ++s) code = (code << 2) | (*s == '0' ? 0u : *s == '1' ? 3u : 1u);
  CHECK_STR(triStateString(code, 24), sym);
  CHECK_STR(triStateString(0x800000, 24), "");  // "10" pair invalid
  auto e = splitCode(Protocol::Ev1527, 0xABCDE5, 24);
  CHECK_EQ(e.address, 0xABCDEu);
  CHECK_EQ(e.data, 5u);
  auto p = splitCode(Protocol::Pt2262, 0xABCD12, 24);
  CHECK_EQ(p.address, 0xABCDu);
  CHECK_EQ(p.data, 0x12u);
}

TEST(rf_codec_duration) {
  auto tx = encodeP1Transmission(0, 24, 350, 1);
  // frame 32T + 24*4T, plus terminating sync 32T
  CHECK_EQ(transmissionDurationUs(tx), 350u * (32 + 96 + 32));
}
