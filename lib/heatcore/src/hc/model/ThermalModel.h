#pragma once
// Learning thermal room model.
//
// Model (per room, sampled every `sampleMin` minutes):
//     dT/dt [K/h] = a * q(t) + b * (Tref - T) + c
//   q    heat input 0..1 derived from the valve open fraction u:
//        radiators: u delayed by the measured dead time,
//        underfloor heating: u through a first-order lag (the screed stores
//        the heat for hours, the room only sees its slow average)
//   Tref outdoor temperature if available, otherwise 0 (then c absorbs b*Tout)
// Parameters are estimated by recursive least squares with forgetting. A bank
// of candidates runs side by side - the dead-time model and lags of 30 min to
// 6 h - and the one with the smallest residual describes the room, so
// radiators and underfloor heating are recognised without configuration.
//
// Every sample is classified before it may be used:
//   Normal        -> used for parameter adaptation
//   Ventilation   -> window open or within the recovery time after closing
//   Incomplete    -> missing / fallback temperature, gaps
//   Manual        -> manual output override or manual setpoint override
//   HeatingFault  -> safety trip, circuit fault, sensor failure, suspected fault
// Only Normal samples change the parameters. An open window is therefore never
// interpreted as poor heating performance.
//
// The model only *advises* the hysteresis controller (bounded threshold
// shifts). When it is not ready or too uncertain the controller runs the
// conventional strategy unchanged.
#include <ArduinoJson.h>

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "hc/core/Clock.h"
#include "hc/core/Event.h"
#include "hc/heating/Heating.h"

namespace hc {

enum class PeriodClass : uint8_t { Normal = 0, Ventilation, Incomplete, Manual, HeatingFault, Count };
const char* periodClassName(PeriodClass c);

enum class ModelStatus : uint8_t { Learning = 0, Ready, Degraded, Disabled };
const char* modelStatusName(ModelStatus s);

struct ThermalInput {  // one-minute observation from the system
  bool tempValid = false;
  float temp = 0.f;
  bool tempFallback = false;
  bool outdoorValid = false;
  float outdoor = 0.f;
  float duty = 0.f;  // 0..1 valve open fraction during the last minute
  bool windowOpen = false;
  bool manualOutput = false;
  bool manualSetpoint = false;
  bool fault = false;
};

struct Prediction {
  bool valid = false;
  float mean = 0.f;
  float stddev = 0.f;
};

struct ModelConfig {
  uint8_t sampleMin = 10;  // slope over 10 one-minute samples (DS18B20 resolution 0.0625 K)
  float forgetting = 0.995f;
  uint16_t minSamples = 48;       // normal samples before the model can be ready
  float maxRelStdA = 0.35f;       // relative uncertainty of the heating gain for "ready"
  uint16_t ventilationRecoveryMin = 30;
  bool learnDuringManualSetpoint = false;
  float maxAdviceK = 0.5f;
  float maxAdviceStd = 0.35f;     // prediction std above which no advice is given
};

class ThermalModel {
 public:
  static constexpr int kParams = 3;
  static constexpr int kLagCount = 6;
  static constexpr float kLagMin[kLagCount] = {30.f, 60.f, 90.f, 150.f, 240.f, 360.f};

  explicit ThermalModel(const ModelConfig* cfg = nullptr);
  void setConfig(const ModelConfig* cfg) { cfg_ = cfg; }
  void reset();

  // Feed one-minute observations. Returns true when a sample interval was
  // completed (classification + possible update happened).
  bool observe(const ThermalInput& in, Millis now);

  // Events that change the learning context (window, manual changes, ...)
  void noteContext(const char* what, Millis now, int64_t epoch);

  Prediction predict(float horizonMin, float futureDuty) const;
  // Bounded threshold advice for the hysteresis controller.
  ModelAdvice advice(bool heatingOn, float currentTemp) const;

  ModelStatus status() const { return status_; }
  float heatingGain() const { return sel().theta[0]; }  // K/h at full (steady) heat input
  float lossCoeff() const { return sel().theta[1]; }    // 1/h
  float offset() const { return sel().theta[2]; }
  float paramStd(int i) const;
  float residualStd() const;
  float trendKh() const { return trend_; }
  // response time: dead time (radiators) or lag time constant (underfloor)
  float delayMin() const { return best_ ? kLagMin[best_ - 1] : delayMean_; }
  float delayStdMin() const;
  bool slowEmitter() const { return best_ > 0; }  // underfloor-like response selected
  bool emitterKnown() const;  // enough data to compare all candidates (or a lag model chosen)
  float lagMin() const { return best_ ? kLagMin[best_ - 1] : 0.f; }
  bool usesOutdoor() const { return haveOutdoor_; }
  uint32_t samples(PeriodClass c) const { return counts_[static_cast<size_t>(c)]; }
  PeriodClass lastClass() const { return lastClass_; }
  bool faultSuspected() const { return faultSuspected_; }
  uint32_t delaySamples() const { return delayCount_; }

  void toJson(JsonObject o, bool runtime) const;
  void fromJson(JsonObjectConst o);

  struct ContextEntry {
    int64_t epoch;
    Millis ms;
    std::string what;
  };
  const std::deque<ContextEntry>& context() const { return context_; }

 private:
  void finishInterval(Millis now);
  struct Cand {  // one candidate input model with its own estimator
    std::array<float, kParams> theta{};
    std::array<std::array<float, kParams>, kParams> P{};
    float sigma2 = 0.04f;  // residual variance (K/h)^2
    uint32_t n = 0;        // updates (a fresh candidate must learn before it may win)
    float q = 0.f;         // lag filter state (lag candidates)
    float qSum = 0.f;      // filter output summed over the interval
    uint16_t qN = 0;
  };
  static void resetCand(Cand& c);
  static void rlsUpdate(Cand& c, const float phi[kParams], float y, float lambda);
  const Cand& sel() const { return cand_[static_cast<size_t>(best_)]; }
  void selectBest();
  void updateStatus();
  float delayedDuty(float delayMin, float offsetMin) const;
  void trackDelay(const ThermalInput& in, Millis now, bool disturbed);

  const ModelConfig* cfg_;
  ModelConfig defaultCfg_;
  // parameters: [0] dead-time model, [1..] lag models
  std::array<Cand, kLagCount + 1> cand_{};
  int best_ = 0;
  float baselineSigma2_ = 0.f;
  std::array<uint32_t, static_cast<size_t>(PeriodClass::Count)> counts_{};
  ModelStatus status_ = ModelStatus::Learning;
  PeriodClass lastClass_ = PeriodClass::Incomplete;
  // interval accumulation
  uint8_t minutes_ = 0;
  std::vector<float> temps_;
  float tSum_ = 0.f, outSum_ = 0.f;
  float lastOutdoor_ = 0.f;
  bool haveOutdoor_ = false;
  uint8_t tCount_ = 0, outCount_ = 0;
  bool haveStart_ = false;
  bool flagVent_ = false, flagIncomplete_ = false, flagManual_ = false, flagFault_ = false;
  Millis lastWindowMs_ = 0;
  bool windowSeen_ = false;
  // duty history, one entry per minute (newest at back), max 240 min
  std::deque<float> dutyHist_;
  float trend_ = 0.f;
  float lastTemp_ = 0.f;
  bool haveLastTemp_ = false;
  // dead time estimation
  bool delayTracking_ = false;
  Millis heatStartMs_ = 0;
  float tMin_ = 0.f;
  Millis tMinMs_ = 0;
  Millis lastHeatOffMs_ = 0;
  bool wasHeating_ = false;
  float delayMean_ = 30.f, delayM2_ = 0.f;
  uint32_t delayCount_ = 0;
  // fault detection
  uint8_t poorResponseCount_ = 0;
  bool faultSuspected_ = false;
  std::deque<ContextEntry> context_;
};

class ThermalModelManager {
 public:
  ThermalModelManager(EventBus& bus, const IClock& clock) : bus_(bus), clock_(clock) {}
  ModelConfig cfg;
  bool enabled = true;

  ThermalModel& model(const std::string& room);
  const ThermalModel* find(const std::string& room) const;
  void removeMissing(const std::vector<Room>& rooms);
  // Called once per minute by the system.
  void observe(const std::string& room, const ThermalInput& in, Millis now);
  void noteContext(const std::string& room, const char* what);
  ModelAdvice advice(const std::string& room, bool heatingOn, float currentTemp) const;
  void reset(const std::string& room);

  void toJson(JsonObject o, bool runtime) const;
  void fromJson(JsonObjectConst o);
  const std::vector<std::pair<std::string, ThermalModel>>& models() const { return models_; }

 private:
  EventBus& bus_;
  const IClock& clock_;
  std::vector<std::pair<std::string, ThermalModel>> models_;
};

}  // namespace hc
