#include "hc/rf/RfTransmitter.h"

#include <cstdio>

#include "hc/core/Log.h"

namespace hc {
namespace rf {

namespace {
constexpr const char* TAG = "rf-tx";
constexpr size_t kHistory = 24;
}  // namespace

RfTransmitter::RfTransmitter(RfRegistry& reg, EventBus& bus, const IClock& clock)
    : reg_(reg), bus_(bus), clock_(clock) {}

uint32_t RfTransmitter::estimateAirtimeMs(uint8_t bits, uint16_t pulseUs, uint8_t repeats) {
  // every frame is 32T sync + 4T per bit, plus one terminating sync
  uint64_t us = static_cast<uint64_t>(pulseUs) * ((32u + 4u * bits) * repeats + 32u);
  return static_cast<uint32_t>((us + 999) / 1000);
}

std::string RfTransmitter::enqueue(const std::string& ref, EventSource origin, uint8_t depth, uint16_t ruleId,
                                   bool forAutomation, std::vector<uint32_t>* jobIds) {
  if (!cfg_.enabled) {
    ++stats_.rejectedDisabled;
    return "Sender deaktiviert";
  }
  std::string err;
  auto targets = reg_.resolveTx(ref, forAutomation, &err);
  if (targets.empty()) return err.empty() ? "Befehl nicht gefunden" : err;
  if (queue_.size() + targets.size() > cfg_.maxQueue) {
    ++stats_.rejectedQueueFull;
    return "Sendewarteschlange voll";
  }
  for (auto& t : targets) {
    TxJob j;
    j.id = nextJob_++;
    j.deviceId = t.device->id;
    j.commandId = t.command->id;
    j.code = t.command->code;
    j.bits = t.command->bits;
    j.pulseUs = t.device->pulseUs;
    uint8_t rep = t.command->repeats ? t.command->repeats : t.device->repeats;
    j.repeats = rep > RfRegistry::kMaxRepeats ? RfRegistry::kMaxRepeats : rep;
    j.enqueuedMs = clock_.nowMs();
    j.timeoutMs = cfg_.defaultTimeoutMs;
    j.origin = origin;
    j.depth = depth;
    j.ruleId = ruleId;
    j.label = t.device->name + "/" + t.command->name;
    queue_.push_back(j);
    ++stats_.queued;
    if (jobIds) jobIds->push_back(j.id);
  }
  return std::string();
}

std::string RfTransmitter::enqueueRaw(uint32_t code, uint8_t bits, uint16_t pulseUs, uint8_t repeats,
                                      const std::string& label, EventSource origin, bool selfTest, uint32_t* jobId) {
  if (!cfg_.enabled) {
    ++stats_.rejectedDisabled;
    return "Sender deaktiviert";
  }
  if (bits != 24) return "Nur 24-Bit-Codes";
  if (pulseUs < 80 || pulseUs > 750) return "Pulslaenge ausserhalb 80..750 us";
  if (repeats < 1 || repeats > RfRegistry::kMaxRepeats) return "Wiederholungen 1..20";
  if (queue_.size() >= cfg_.maxQueue) {
    ++stats_.rejectedQueueFull;
    return "Sendewarteschlange voll";
  }
  TxJob j;
  j.id = nextJob_++;
  j.code = code;
  j.bits = bits;
  j.pulseUs = pulseUs;
  j.repeats = repeats;
  j.enqueuedMs = clock_.nowMs();
  j.timeoutMs = cfg_.defaultTimeoutMs;
  j.origin = origin;
  j.selfTest = selfTest;
  j.label = label;
  queue_.push_back(j);
  ++stats_.queued;
  if (jobId) *jobId = j.id;
  return std::string();
}

void RfTransmitter::prune(Millis now) const {
  while (!airtime_.empty() && elapsed(now, airtime_.front().first) > 3600u * 1000u) airtime_.pop_front();
}

uint32_t RfTransmitter::airtimeLastHourMs() const {
  prune(clock_.nowMs());
  uint32_t sum = 0;
  for (auto& a : airtime_) sum += a.second;
  return sum;
}

uint32_t RfTransmitter::budgetMs() const {
  float pct = cfg_.dutyCyclePct;
  if (pct > 10.f) pct = 10.f;
  if (pct < 0.f) pct = 0.f;
  return static_cast<uint32_t>(pct / 100.f * 3600.f * 1000.f);
}

bool RfTransmitter::takeJob(TxJob& job, std::vector<Pulse>& pulses) {
  if (busy_) return false;
  Millis now = clock_.nowMs();
  while (!queue_.empty()) {
    TxJob& j = queue_.front();
    if (elapsed(now, j.enqueuedMs) > j.timeoutMs) {
      TxJob dead = j;
      queue_.pop_front();
      ++stats_.rejectedTimeout;
      finishFailed(dead, "Timeout in der Warteschlange");
      continue;
    }
    uint32_t air = estimateAirtimeMs(j.bits, j.pulseUs, j.repeats);
    if (airtimeLastHourMs() + air > budgetMs()) {
      TxJob dead = j;
      queue_.pop_front();
      ++stats_.rejectedDuty;
      finishFailed(dead, "Duty-Cycle-Budget erschoepft");
      continue;
    }
    if (haveLast_ && elapsed(now, lastEnd_) < cfg_.minIntervalMs) return false;  // try again later
    job = j;
    queue_.pop_front();
    pulses = encodeP1Transmission(job.code, job.bits, job.pulseUs, job.repeats);
    busy_ = true;
    // book air time up front so concurrent jobs see the reservation
    airtime_.push_back({now, air});
    return true;
  }
  return false;
}

void RfTransmitter::complete(const TxJob& job, bool ok, uint32_t airtimeMs, const char* reason) {
  busy_ = false;
  lastEnd_ = clock_.nowMs();
  haveLast_ = true;
  if (ok) ++stats_.sent;
  else {
    ++stats_.failed;
    stats_.lastError = reason ? reason : "Fehler";
  }
  if (job.deviceId) {
    if (TxDevice* d = reg_.txDevice(job.deviceId)) {
      if (TxCommand* c = d->command(job.commandId)) {
        if (ok) ++c->sent;
        else ++c->failed;
        c->lastEpoch = clock_.epoch();
      }
    }
  }
  record(job, ok, airtimeMs, reason ? reason : "");
  Event e(ok ? EventType::RfSent : EventType::RfSendFailed, EventSource::Rf);
  e.ts = clock_.nowMs();
  e.a = job.deviceId;
  e.b = job.commandId;
  e.depth = job.depth;
  e.ruleId = job.ruleId;
  e.setText(job.label.c_str());
  e.value = static_cast<float>(airtimeMs);
  bus_.post(e);
  if (!ok) HC_LOGW(TAG, "send '%s' failed: %s", job.label.c_str(), reason ? reason : "?");
}

void RfTransmitter::finishFailed(const TxJob& job, const char* reason) {
  ++stats_.failed;
  stats_.lastError = reason;
  record(job, false, 0, reason);
  Event e(EventType::RfSendFailed, EventSource::Rf);
  e.ts = clock_.nowMs();
  e.a = job.deviceId;
  e.b = job.commandId;
  e.depth = job.depth;
  e.ruleId = job.ruleId;
  e.setText(job.label.c_str());
  bus_.post(e);
  HC_LOGW(TAG, "job '%s' rejected: %s", job.label.c_str(), reason);
}

void RfTransmitter::record(const TxJob& job, bool ok, uint32_t airtimeMs, const std::string& reason) {
  TxRecord r;
  r.jobId = job.id;
  r.epoch = clock_.epoch();
  r.ms = clock_.nowMs();
  r.ok = ok;
  r.airtimeMs = airtimeMs;
  r.label = job.label;
  r.reason = reason;
  r.origin = job.origin;
  if (history_.size() >= kHistory) history_.pop_front();
  history_.push_back(std::move(r));
}

}  // namespace rf
}  // namespace hc
