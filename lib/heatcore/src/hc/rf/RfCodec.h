#pragma once
// RF433 ASK/OOK codec: pulse segmentation, decoding and encoding.
//
// Implemented and tested protocols (see docs/rf433.md):
//   * EV1527  - 24 bit fixed code: 20 bit address + 4 data bits,
//               sync 1T high / 31T low, "0" = 1T/3T, "1" = 3T/1T.
//   * PT2262  - 12 tri-state symbols on the same timing family. Each symbol
//               is two bits: 0 = "00", 1 = "11", F (float) = "01".
//
// Both share one waveform; pulse length T is estimated per frame from the
// sync gap, so different oscillator resistors (T approx. 100..700 us) work.
// No other remote protocol is claimed as supported.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hc {
namespace rf {

// One level period as measured on the receiver data pin.
struct Pulse {
  uint16_t us = 0;    // duration in microseconds (saturates at 65535)
  uint8_t level = 0;  // 1 = carrier present (high), 0 = gap
};

enum class Protocol : uint8_t { Unknown = 0, Ev1527, Pt2262, Count };
const char* protocolName(Protocol p);
Protocol protocolFromName(const char* s);

struct DecodedFrame {
  Protocol protocol = Protocol::Unknown;  // classification hint, see pt2262Valid
  uint32_t code = 0;
  uint8_t bits = 0;
  uint16_t pulseUs = 0;      // estimated base pulse length T
  uint8_t quality = 0;       // 0..100 timing quality of this frame
  bool pt2262Valid = false;  // all symbol pairs are valid tri-state symbols
};

// Formats a 24 bit code as PT2262 tri-state string ("01F0...") if valid.
std::string triStateString(uint32_t code, uint8_t bits);

struct CodecConfig {
  uint16_t glitchUs = 60;       // pulses shorter than this are merged (noise)
  uint16_t boundaryUs = 2800;   // low pulses longer than this end a segment
  uint16_t minPulseUs = 80;     // allowed T range for the P1 timing family
  uint16_t maxPulseUs = 750;
  float maxTimingError = 0.45f; // max relative deviation of a single pulse
};

// Splits a continuous pulse stream into segments delimited by long gaps and
// hands complete segments to registered consumers. Also removes glitches.
class Segmenter {
 public:
  using SegmentFn = void (*)(void* ctx, const Pulse* p, size_t n, uint32_t leadGapUs, uint32_t tailGapUs);
  explicit Segmenter(const CodecConfig& cfg) : cfg_(cfg) { buf_.reserve(kMaxSegment); }
  void setConsumer(SegmentFn fn, void* ctx) { fn_ = fn; ctx_ = ctx; }
  void feed(const Pulse* p, size_t n);
  // Flush a pending segment when the line is quiet (no edge for > boundary).
  void flushIdle();
  uint32_t segments() const { return segments_; }
  uint32_t overflows() const { return overflows_; }
  uint32_t glitches() const { return glitches_; }
  static constexpr size_t kMaxSegment = 160;

 private:
  void push(Pulse p);
  void emit(uint32_t tailGapUs);
  const CodecConfig& cfg_;
  std::vector<Pulse> buf_;
  uint32_t leadGap_ = 0;
  bool havePending_ = false;
  bool mergeNext_ = false;
  Pulse pending_{};
  SegmentFn fn_ = nullptr;
  void* ctx_ = nullptr;
  uint32_t segments_ = 0, overflows_ = 0, glitches_ = 0;
};

// Decodes one segment of the EV1527/PT2262 timing family.
// leadGapUs is the sync gap that preceded the segment (31 T).
bool decodeP1Segment(const CodecConfig& cfg, const Pulse* p, size_t n, uint32_t leadGapUs, DecodedFrame& out);

// Builds one frame (sync + data) for the transmitter. The waveform is
// [H 1T][L 31T][data bits]; the caller repeats it and appends a final sync.
void encodeP1Frame(uint32_t code, uint8_t bits, uint16_t pulseUs, std::vector<Pulse>& out);
// Complete transmission: `repeats` frames plus a terminating sync pulse.
std::vector<Pulse> encodeP1Transmission(uint32_t code, uint8_t bits, uint16_t pulseUs, uint8_t repeats);
uint32_t transmissionDurationUs(const std::vector<Pulse>& pulses);

// Address/data split helpers for device grouping.
struct CodeSplit {
  uint32_t address = 0;
  uint32_t data = 0;
  uint8_t addressBits = 0;
};
CodeSplit splitCode(Protocol p, uint32_t code, uint8_t bits);

}  // namespace rf
}  // namespace hc
