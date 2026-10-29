#pragma once
// RF433 transmit service. Jobs are queued by any context (web, automation,
// MQTT) and executed by a dedicated low-priority task, so a transmission
// never blocks heating control or sensor acquisition.
//
// Regulatory guard (433.05-434.79 MHz SRD band, EU ERC/REC 70-03 / EN 300 220,
// in Germany the BNetzA general assignment for SRDs): the transmitter keeps a
// rolling one-hour air-time budget (default 1 %, max. 10 % duty cycle),
// limits repeats per job and enforces a minimum interval between jobs.
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "hc/core/Clock.h"
#include "hc/core/Event.h"
#include "hc/rf/RfCodec.h"
#include "hc/rf/RfDevices.h"

namespace hc {
namespace rf {

class ITxBackend {
 public:
  virtual ~ITxBackend() = default;
  virtual bool ready() const = 0;
  // Emits the pulse train; returns false on driver error or timeout.
  virtual bool transmit(const std::vector<Pulse>& pulses, uint32_t timeoutMs) = 0;
};

struct TxJob {
  uint32_t id = 0;
  uint16_t deviceId = 0;
  uint16_t commandId = 0;
  uint32_t code = 0;
  uint8_t bits = 24;
  uint16_t pulseUs = 350;
  uint8_t repeats = 8;
  Millis enqueuedMs = 0;
  uint32_t timeoutMs = 5000;
  EventSource origin = EventSource::System;
  uint8_t depth = 0;
  uint16_t ruleId = 0;
  bool selfTest = false;
  std::string label;
};

struct TxRecord {
  uint32_t jobId = 0;
  int64_t epoch = 0;
  Millis ms = 0;
  bool ok = false;
  uint32_t airtimeMs = 0;
  std::string label;
  std::string reason;
  EventSource origin = EventSource::System;
};

struct TxConfig {
  bool enabled = true;
  float dutyCyclePct = 1.0f;     // of a rolling hour, max 10
  uint16_t minIntervalMs = 250;  // between two jobs
  uint32_t defaultTimeoutMs = 5000;
  uint8_t maxQueue = 8;
};

struct TxStats {
  uint32_t queued = 0, sent = 0, failed = 0;
  uint32_t rejectedDuty = 0, rejectedTimeout = 0, rejectedQueueFull = 0, rejectedDisabled = 0;
  std::string lastError;
};

class RfTransmitter {
 public:
  RfTransmitter(RfRegistry& reg, EventBus& bus, const IClock& clock);

  // Queue command(s) referenced as "<dev>/<cmd>" or "group:<g>/<name>".
  // Returns empty string on success, otherwise the reason.
  std::string enqueue(const std::string& ref, EventSource origin, uint8_t depth, uint16_t ruleId, bool forAutomation,
                      std::vector<uint32_t>* jobIds = nullptr);
  std::string enqueueRaw(uint32_t code, uint8_t bits, uint16_t pulseUs, uint8_t repeats, const std::string& label,
                         EventSource origin, bool selfTest, uint32_t* jobId = nullptr);

  // Called by the TX task (under the system lock). Returns true with a job
  // ready to transmit; expired / over-budget jobs are completed as failed.
  bool takeJob(TxJob& job, std::vector<Pulse>& pulses);
  void complete(const TxJob& job, bool ok, uint32_t airtimeMs, const char* reason);

  uint32_t airtimeLastHourMs() const;
  uint32_t budgetMs() const;
  bool busy() const { return busy_; }
  size_t queueLength() const { return queue_.size(); }
  const std::deque<TxRecord>& history() const { return history_; }
  TxConfig& config() { return cfg_; }
  const TxConfig& config() const { return cfg_; }
  const TxStats& stats() const { return stats_; }
  // Pure estimate for UI/validation.
  static uint32_t estimateAirtimeMs(uint8_t bits, uint16_t pulseUs, uint8_t repeats);

 private:
  void prune(Millis now) const;
  void record(const TxJob& job, bool ok, uint32_t airtimeMs, const std::string& reason);
  void finishFailed(const TxJob& job, const char* reason);

  RfRegistry& reg_;
  EventBus& bus_;
  const IClock& clock_;
  TxConfig cfg_;
  TxStats stats_;
  std::deque<TxJob> queue_;
  mutable std::deque<std::pair<Millis, uint32_t>> airtime_;  // start, ms
  std::deque<TxRecord> history_;
  uint32_t nextJob_ = 1;
  Millis lastEnd_ = 0;
  bool haveLast_ = false;
  bool busy_ = false;
};

}  // namespace rf
}  // namespace hc
