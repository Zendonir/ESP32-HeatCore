#pragma once
// RF433 receive pipeline: pulses -> segments -> frames -> presses -> events.
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "hc/core/Clock.h"
#include "hc/core/Event.h"
#include "hc/rf/RfCodec.h"
#include "hc/rf/RfDevices.h"
#include "hc/rf/RfSensors.h"

namespace hc {
namespace rf {

enum class FrameStatus : uint8_t { Known, Unknown, Ignored, Disabled, TimingMismatch, OwnEcho, Unsupported };
const char* frameStatusName(FrameStatus s);

struct MonitorEntry {
  uint32_t seq = 0;
  Millis ts = 0;
  int64_t epoch = 0;
  uint32_t code = 0;
  uint8_t bits = 0;
  uint16_t pulseUs = 0;
  uint8_t quality = 0;
  bool pt2262 = false;
  int32_t device = -1;
  int16_t button = -1;
  FrameStatus status = FrameStatus::Unknown;
};

struct UnknownCode {
  uint32_t code = 0;
  uint8_t bits = 0;
  float pulseUs = 0.f;
  float quality = 0.f;
  uint32_t frames = 0;
  uint32_t presses = 0;
  Millis firstMs = 0, lastMs = 0;
  int64_t lastEpoch = 0;
  bool pt2262 = false;
};

struct LearnCandidate {
  uint32_t code = 0;
  uint8_t bits = 0;
  float pulseUs = 0.f;
  float quality = 0.f;
  uint32_t frames = 0;
  uint32_t presses = 0;
  Millis firstMs = 0, lastMs = 0;
  bool pt2262 = false;
  int32_t knownDevice = -1;
  int16_t knownButton = -1;
  // Reliable: seen in >= 2 separate presses with >= 2 frames each and decent timing.
  bool reliable() const { return presses >= 2 && frames >= presses * 2 && quality >= 50.f; }
};

struct RawCapture {
  Millis ts = 0;
  uint32_t leadGapUs = 0;
  bool decoded = false;
  std::vector<Pulse> pulses;
};

struct RxStats {
  uint32_t frames = 0;
  uint32_t known = 0;
  uint32_t unknown = 0;
  uint32_t ignored = 0;
  uint32_t disabled = 0;
  uint32_t unsupported = 0;     // decodable P1 frames with bit counts != 24
  uint32_t timingMismatch = 0;
  uint32_t presses = 0;
  uint32_t rejected = 0;        // presses with too few repeats (debounce)
  uint32_t debounced = 0;       // frames inside the retrigger window
  uint32_t echoes = 0;          // own transmissions received back
  uint32_t sensorRows = 0;
  float qualityAvg = 0.f;
  Millis lastFrameMs = 0;
  // filled by the platform ISR layer
  uint32_t isrEdges = 0;
  uint32_t droppedPulses = 0;
  uint32_t edgesPerSec = 0;
};

class RfReceiver {
 public:
  RfReceiver(RfRegistry& reg, EventBus& bus, const IClock& clock);

  void setSensorHub(RfSensorHub* hub) { sensors_ = hub; }
  void feed(const Pulse* p, size_t n);
  void idle();  // line quiet: flush pending segment
  void tick();  // releases, long presses, supervision, learn timeout

  // Suppress normal processing while the own transmitter is active.
  // Received copies of `expectCode` count as loopback echoes.
  void suppressFor(Millis durationMs, uint32_t expectCode, uint8_t bits);
  bool suppressed() const;
  uint32_t echoesSince(uint32_t mark) const { return stats_.echoes - mark; }

  // Learn mode
  void startLearn(uint32_t timeoutSec);
  void stopLearn();
  bool learning() const { return learning_; }
  Millis learnRemainingMs() const;
  const std::vector<LearnCandidate>& candidates() const { return candidates_; }

  // Diagnostics
  void startRawCapture(uint8_t count) { rawRemaining_ = count; }
  uint8_t rawCaptureRemaining() const { return rawRemaining_; }
  const std::deque<RawCapture>& rawCaptures() const { return raw_; }
  std::vector<MonitorEntry> monitor(uint32_t sinceSeq, size_t maxCount) const;
  uint32_t monitorSeq() const { return monitorSeq_; }
  const std::vector<UnknownCode>& unknownCodes() const { return unknown_; }
  void clearUnknown() { unknown_.clear(); }
  void forgetUnknown(uint32_t code, uint8_t bits);

  RxStats& stats() { return stats_; }
  const RxStats& stats() const { return stats_; }
  CodecConfig& codec() { return cfg_; }
  const Segmenter& segmenter() const { return seg_; }

  bool enabled = true;
  uint16_t unknownMinRepeats = 2;

 private:
  struct PressState {
    uint32_t code;
    uint8_t bits;
    Millis first, last;
    uint16_t frames;
    bool pressed, longSent;
    int32_t device;  // -1 unknown code
    uint8_t button;
  };

  static void segmentTrampoline(void* ctx, const Pulse* p, size_t n, uint32_t lead, uint32_t tail);
  void onSegment(const Pulse* p, size_t n, uint32_t lead, uint32_t tail);
  void onFrame(const DecodedFrame& f);
  void trackPress(const DecodedFrame& f, RxDevice* dev, RxButton* btn);
  void pressStarted(PressState& ps);
  void pressReleased(PressState& ps);
  void emit(RxDevice& dev, RxButton* btn, Gesture g, float quality);
  void setState(RxDevice& dev, RxButton* btn, int8_t state);
  void updateUnknown(const DecodedFrame& f, bool newPress);
  void updateLearn(const DecodedFrame& f, int32_t dev, int16_t btn);
  void pushMonitor(const DecodedFrame& f, FrameStatus st, int32_t dev, int16_t btn);
  void captureRaw(const Pulse* p, size_t n, uint32_t lead, bool decoded);

  RfRegistry& reg_;
  EventBus& bus_;
  const IClock& clock_;
  CodecConfig cfg_;
  Segmenter seg_;
  RfSensorHub* sensors_ = nullptr;
  RxStats stats_;
  std::vector<PressState> presses_;
  std::vector<UnknownCode> unknown_;
  std::vector<LearnCandidate> candidates_;
  std::deque<MonitorEntry> monitor_;
  std::deque<RawCapture> raw_;
  uint32_t monitorSeq_ = 0;
  uint8_t rawRemaining_ = 0;
  bool learning_ = false;
  Millis learnStart_ = 0, learnTimeoutMs_ = 0;
  Millis suppressUntil_ = 0;
  bool suppressActive_ = false;
  uint32_t expectCode_ = 0;
  uint8_t expectBits_ = 0;
  Millis bootMs_ = 0;
};

}  // namespace rf
}  // namespace hc
