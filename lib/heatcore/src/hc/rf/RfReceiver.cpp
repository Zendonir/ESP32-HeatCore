#include "hc/rf/RfReceiver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "hc/core/Log.h"

namespace hc {
namespace rf {

namespace {
constexpr size_t kMaxPressStates = 16;
constexpr size_t kMaxUnknown = 32;
constexpr size_t kMaxCandidates = 16;
constexpr size_t kMonitorSize = 64;
constexpr size_t kRawSize = 6;
constexpr const char* TAG = "rf-rx";

inline float ema(float prev, float v, float alpha, bool first) { return first ? v : prev + alpha * (v - prev); }
}  // namespace

const char* frameStatusName(FrameStatus s) {
  switch (s) {
    case FrameStatus::Known: return "known";
    case FrameStatus::Unknown: return "unknown";
    case FrameStatus::Ignored: return "ignored";
    case FrameStatus::Disabled: return "disabled";
    case FrameStatus::TimingMismatch: return "timing_mismatch";
    case FrameStatus::OwnEcho: return "own_echo";
    case FrameStatus::Unsupported: return "unsupported";
  }
  return "?";
}

RfReceiver::RfReceiver(RfRegistry& reg, EventBus& bus, const IClock& clock)
    : reg_(reg), bus_(bus), clock_(clock), seg_(cfg_) {
  seg_.setConsumer(&RfReceiver::segmentTrampoline, this);
  bootMs_ = clock_.nowMs();
}

void RfReceiver::segmentTrampoline(void* ctx, const Pulse* p, size_t n, uint32_t lead, uint32_t tail) {
  static_cast<RfReceiver*>(ctx)->onSegment(p, n, lead, tail);
}

void RfReceiver::feed(const Pulse* p, size_t n) {
  if (!enabled) return;
  seg_.feed(p, n);
}

void RfReceiver::idle() { seg_.flushIdle(); }

void RfReceiver::suppressFor(Millis durationMs, uint32_t expectCode, uint8_t bits) {
  suppressUntil_ = clock_.nowMs() + durationMs;
  suppressActive_ = true;
  expectCode_ = expectCode;
  expectBits_ = bits;
}

bool RfReceiver::suppressed() const { return suppressActive_ && !reached(clock_.nowMs(), suppressUntil_); }

void RfReceiver::onSegment(const Pulse* p, size_t n, uint32_t lead, uint32_t tail) {
  (void)tail;
  DecodedFrame f;
  bool decoded = decodeP1Segment(cfg_, p, n, lead, f);
  bool sensor = false;
  if (!decoded && sensors_) {
    sensor = sensors_->onSegment(p, n, clock_.nowMs());
    if (sensor) ++stats_.sensorRows;
  }
  if (rawRemaining_ > 0 || (n >= 16 && !decoded && !sensor)) {
    // keep a few undecodable bursts for the diagnosis page (protocol-independent)
    if (rawRemaining_ > 0 || raw_.size() < kRawSize) captureRaw(p, n, lead, decoded || sensor);
  }
  if (decoded) onFrame(f);
}

void RfReceiver::captureRaw(const Pulse* p, size_t n, uint32_t lead, bool decoded) {
  if (rawRemaining_ > 0) --rawRemaining_;
  RawCapture c;
  c.ts = clock_.nowMs();
  c.leadGapUs = lead;
  c.decoded = decoded;
  c.pulses.assign(p, p + n);
  if (raw_.size() >= kRawSize) raw_.pop_front();
  raw_.push_back(std::move(c));
}

void RfReceiver::pushMonitor(const DecodedFrame& f, FrameStatus st, int32_t dev, int16_t btn) {
  MonitorEntry m;
  m.seq = ++monitorSeq_;
  m.ts = clock_.nowMs();
  m.epoch = clock_.epoch();
  m.code = f.code;
  m.bits = f.bits;
  m.pulseUs = f.pulseUs;
  m.quality = f.quality;
  m.pt2262 = f.pt2262Valid;
  m.device = dev;
  m.button = btn;
  m.status = st;
  if (monitor_.size() >= kMonitorSize) monitor_.pop_front();
  monitor_.push_back(m);
}

std::vector<MonitorEntry> RfReceiver::monitor(uint32_t sinceSeq, size_t maxCount) const {
  std::vector<MonitorEntry> out;
  for (auto it = monitor_.rbegin(); it != monitor_.rend() && out.size() < maxCount; ++it) {
    if (it->seq <= sinceSeq) break;
    out.push_back(*it);
  }
  std::reverse(out.begin(), out.end());
  return out;
}

void RfReceiver::onFrame(const DecodedFrame& f) {
  Millis now = clock_.nowMs();
  ++stats_.frames;
  stats_.qualityAvg = ema(stats_.qualityAvg, f.quality, 0.05f, stats_.frames == 1);
  stats_.lastFrameMs = now;

  if (suppressed()) {
    if (f.code == expectCode_ && f.bits == expectBits_) ++stats_.echoes;
    pushMonitor(f, FrameStatus::OwnEcho, -1, -1);
    return;
  }
  if (f.bits != 24) {
    ++stats_.unsupported;
    pushMonitor(f, FrameStatus::Unsupported, -1, -1);
    return;
  }

  ButtonRef ref = reg_.findByCode(f.code, f.bits);
  int32_t devId = ref.device ? ref.device->id : -1;
  int16_t btnIdx = ref.button ? ref.button->index : -1;
  if (learning_) updateLearn(f, devId, btnIdx);

  if (ref.device) {
    RxDevice& d = *ref.device;
    if (!d.enabled) {
      ++stats_.disabled;
      pushMonitor(f, FrameStatus::Disabled, devId, btnIdx);
      return;
    }
    float dev = std::fabs(static_cast<float>(f.pulseUs) - d.pulseUs) / d.pulseUs;
    if (dev > 0.4f) {
      // Same bit pattern but clearly different transmitter timing: likely a
      // different device or a corrupted frame. Never trigger actions on it.
      ++stats_.timingMismatch;
      ++d.stats.timingMismatch;
      pushMonitor(f, FrameStatus::TimingMismatch, devId, btnIdx);
      return;
    }
    ++stats_.known;
    bool first = !d.stats.seen;
    ++d.stats.frames;
    d.stats.seen = true;
    d.stats.lastSeenMs = now;
    d.stats.lastEpoch = clock_.epoch();
    d.stats.qualityAvg = ema(d.stats.qualityAvg, f.quality, 0.1f, first);
    d.stats.pulseUsAvg = ema(d.stats.pulseUsAvg, f.pulseUs, 0.1f, first);
    if (!d.available) {
      d.available = true;
      emit(d, nullptr, Gesture::Available, f.quality);
    }
    pushMonitor(f, FrameStatus::Known, devId, btnIdx);
    trackPress(f, &d, ref.button);
    return;
  }
  if (reg_.isIgnored(f.code, f.bits)) {
    ++stats_.ignored;
    pushMonitor(f, FrameStatus::Ignored, -1, -1);
    return;
  }
  ++stats_.unknown;
  pushMonitor(f, FrameStatus::Unknown, -1, -1);
  trackPress(f, nullptr, nullptr);
}

void RfReceiver::trackPress(const DecodedFrame& f, RxDevice* dev, RxButton* btn) {
  Millis now = clock_.nowMs();
  uint16_t releaseMs = dev ? dev->releaseMs : 250;
  for (auto& ps : presses_) {
    if (ps.code == f.code && ps.bits == f.bits && elapsed(now, ps.last) <= releaseMs) {
      ps.last = now;
      ++ps.frames;
      uint16_t need = dev ? dev->minRepeats : unknownMinRepeats;
      if (!ps.pressed && ps.frames >= need) pressStarted(ps);
      if (!dev) updateUnknown(f, false);
      return;
    }
  }
  // New press. Debounce: frames right after a release are a bounce/echo.
  if (btn && btn->everReleased && elapsed(now, btn->lastReleaseMs) < dev->retriggerMs) {
    ++stats_.debounced;
    btn->lastReleaseMs = now;
    return;
  }
  if (presses_.size() >= kMaxPressStates) presses_.erase(presses_.begin());
  PressState ps{f.code, f.bits, now, now, 1, false, false, dev ? static_cast<int32_t>(dev->id) : -1,
                btn ? btn->index : static_cast<uint8_t>(0)};
  presses_.push_back(ps);
  if (!dev) updateUnknown(f, false);
  uint16_t need = dev ? dev->minRepeats : unknownMinRepeats;
  if (ps.frames >= need) pressStarted(presses_.back());
}

void RfReceiver::pressStarted(PressState& ps) {
  ps.pressed = true;
  ++stats_.presses;
  if (ps.device < 0) {
    for (auto& u : unknown_)
      if (u.code == ps.code && u.bits == ps.bits) ++u.presses;
    if (learning_)
      for (auto& c : candidates_)
        if (c.code == ps.code && c.bits == ps.bits) ++c.presses;
    Event e(EventType::RfUnknownCode, EventSource::Rf);
    e.ts = clock_.nowMs();
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%06lX", static_cast<unsigned long>(ps.code));
    e.setRef(buf);
    e.value = ps.bits;
    bus_.post(e);
    HC_LOGI(TAG, "unknown code %s (%u bit)", buf, ps.bits);
    return;
  }
  RxDevice* d = reg_.rxDevice(static_cast<uint16_t>(ps.device));
  if (!d) return;
  RxButton* b = d->button(ps.button);
  if (!b) return;
  if (learning_)
    for (auto& c : candidates_)
      if (c.code == ps.code && c.bits == ps.bits) ++c.presses;
  ++d->stats.presses;
  ++b->count;
  b->lastEpoch = clock_.epoch();
  switch (b->role) {
    case ButtonRole::Button: emit(*d, b, Gesture::Press, d->stats.qualityAvg); break;
    case ButtonRole::StateOn: setState(*d, b, 1); break;
    case ButtonRole::StateOff: setState(*d, b, 0); break;
  }
}

void RfReceiver::pressReleased(PressState& ps) {
  if (ps.device < 0) return;
  RxDevice* d = reg_.rxDevice(static_cast<uint16_t>(ps.device));
  if (!d) return;
  RxButton* b = d->button(ps.button);
  if (!b) return;
  b->lastReleaseMs = clock_.nowMs();
  b->everReleased = true;
  d->stats.framesPerPress = ema(d->stats.framesPerPress, ps.frames, 0.2f, d->stats.presses <= 1);
  if (b->role != ButtonRole::Button) return;
  if (!ps.longSent) emit(*d, b, Gesture::Short, d->stats.qualityAvg);
  emit(*d, b, Gesture::Release, d->stats.qualityAvg);
}

void RfReceiver::setState(RxDevice& d, RxButton* b, int8_t state) {
  if (state == 1) d.stateOnMs = clock_.nowMs();
  if (d.state == state) return;  // repeated state telegram: no change event
  d.state = state;
  emit(d, b, state ? Gesture::Open : Gesture::Closed, d.stats.qualityAvg);
}

void RfReceiver::emit(RxDevice& d, RxButton* b, Gesture g, float quality) {
  Event e(EventType::InputGesture, EventSource::Rf);
  e.ts = clock_.nowMs();
  e.gesture = g;
  e.a = d.id;
  e.b = b ? b->index : -1;
  char ref[24];
  if (b) std::snprintf(ref, sizeof(ref), "rf:%u/%u", d.id, b->index);
  else std::snprintf(ref, sizeof(ref), "rf:%u", d.id);
  e.setRef(ref);
  e.value = quality;
  e.value2 = static_cast<float>(d.state);
  if (!bus_.post(e)) HC_LOGW(TAG, "event queue full, RF event dropped");
}

void RfReceiver::tick() {
  Millis now = clock_.nowMs();
  if (suppressActive_ && reached(now, suppressUntil_)) suppressActive_ = false;
  // releases and long presses
  for (size_t i = 0; i < presses_.size();) {
    PressState& ps = presses_[i];
    RxDevice* d = ps.device >= 0 ? reg_.rxDevice(static_cast<uint16_t>(ps.device)) : nullptr;
    uint16_t releaseMs = d ? d->releaseMs : 250;
    if (elapsed(now, ps.last) > releaseMs) {
      if (ps.pressed) {
        pressReleased(ps);
      } else {
        ++stats_.rejected;
        if (d) ++d->stats.rejected;
      }
      presses_.erase(presses_.begin() + static_cast<long>(i));
      continue;
    }
    if (d && ps.pressed && !ps.longSent && elapsed(now, ps.first) >= d->longPressMs) {
      RxButton* b = d->button(ps.button);
      if (b && b->role == ButtonRole::Button) {
        ps.longSent = true;
        emit(*d, b, Gesture::Long, d->stats.qualityAvg);
      }
    }
    ++i;
  }
  // supervision and auto-off
  for (auto& d : reg_.rx) {
    if (!d.enabled) continue;
    if (d.supervisionSec) {
      Millis ref = d.stats.seen ? d.stats.lastSeenMs : bootMs_;
      if (d.available && elapsed(now, ref) > d.supervisionSec * 1000u) {
        d.available = false;
        HC_LOGW(TAG, "device '%s' supervision timeout", d.name.c_str());
        emit(d, nullptr, Gesture::Unavailable, 0.f);
      }
    }
    if (d.autoOffSec && d.state == 1 && elapsed(now, d.stateOnMs) > d.autoOffSec * 1000u) {
      d.state = 0;
      emit(d, nullptr, Gesture::Closed, d.stats.qualityAvg);
    }
  }
  if (learning_ && elapsed(now, learnStart_) > learnTimeoutMs_) learning_ = false;
}

void RfReceiver::updateUnknown(const DecodedFrame& f, bool newPress) {
  Millis now = clock_.nowMs();
  UnknownCode* u = nullptr;
  for (auto& x : unknown_)
    if (x.code == f.code && x.bits == f.bits) u = &x;
  if (!u) {
    if (unknown_.size() >= kMaxUnknown) {
      auto oldest = std::min_element(unknown_.begin(), unknown_.end(),
                                     [](const UnknownCode& a, const UnknownCode& b) { return a.lastMs < b.lastMs; });
      unknown_.erase(oldest);
    }
    UnknownCode n;
    n.code = f.code;
    n.bits = f.bits;
    n.firstMs = now;
    n.pt2262 = f.pt2262Valid;
    unknown_.push_back(n);
    u = &unknown_.back();
  }
  bool first = u->frames == 0;
  ++u->frames;
  if (newPress) ++u->presses;
  u->pulseUs = ema(u->pulseUs, f.pulseUs, 0.2f, first);
  u->quality = ema(u->quality, f.quality, 0.2f, first);
  u->lastMs = now;
  u->lastEpoch = clock_.epoch();
}

void RfReceiver::forgetUnknown(uint32_t code, uint8_t bits) {
  unknown_.erase(std::remove_if(unknown_.begin(), unknown_.end(),
                                [&](const UnknownCode& u) { return u.code == code && u.bits == bits; }),
                 unknown_.end());
}

void RfReceiver::startLearn(uint32_t timeoutSec) {
  if (timeoutSec < 10) timeoutSec = 10;
  if (timeoutSec > 600) timeoutSec = 600;
  learning_ = true;
  learnStart_ = clock_.nowMs();
  learnTimeoutMs_ = timeoutSec * 1000u;
  candidates_.clear();
}

void RfReceiver::stopLearn() { learning_ = false; }

Millis RfReceiver::learnRemainingMs() const {
  if (!learning_) return 0;
  Millis e = elapsed(clock_.nowMs(), learnStart_);
  return e >= learnTimeoutMs_ ? 0 : learnTimeoutMs_ - e;
}

void RfReceiver::updateLearn(const DecodedFrame& f, int32_t dev, int16_t btn) {
  Millis now = clock_.nowMs();
  LearnCandidate* c = nullptr;
  for (auto& x : candidates_)
    if (x.code == f.code && x.bits == f.bits) c = &x;
  if (!c) {
    if (candidates_.size() >= kMaxCandidates) {
      auto weakest = std::min_element(candidates_.begin(), candidates_.end(),
                                      [](const LearnCandidate& a, const LearnCandidate& b) { return a.frames < b.frames; });
      candidates_.erase(weakest);
    }
    LearnCandidate n;
    n.code = f.code;
    n.bits = f.bits;
    n.firstMs = now;
    n.pt2262 = f.pt2262Valid;
    candidates_.push_back(n);
    c = &candidates_.back();
  }
  bool first = c->frames == 0;
  ++c->frames;
  c->pulseUs = ema(c->pulseUs, f.pulseUs, 0.2f, first);
  c->quality = ema(c->quality, f.quality, 0.2f, first);
  c->lastMs = now;
  c->knownDevice = dev;
  c->knownButton = btn;
}

}  // namespace rf
}  // namespace hc
