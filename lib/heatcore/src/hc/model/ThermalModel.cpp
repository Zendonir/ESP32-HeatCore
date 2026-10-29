#include "hc/model/ThermalModel.h"

#include <algorithm>
#include <cmath>

namespace hc {

namespace {
const char* const kPeriodNames[] = {"normal", "ventilation", "incomplete", "manual", "heating_fault"};
const char* const kStatusNames[] = {"learning", "ready", "degraded", "disabled"};
constexpr size_t kDutyHistory = 240;
constexpr float kPInit = 50.f;
constexpr float kPMax = 1000.f;

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// least-squares slope of equally spaced samples (per sample step)
float slope(const std::vector<float>& y) {
  size_t n = y.size();
  if (n < 2) return 0.f;
  float mx = (n - 1) / 2.f, my = 0.f;
  for (float v : y) my += v;
  my /= n;
  float num = 0.f, den = 0.f;
  for (size_t i = 0; i < n; ++i) {
    num += (i - mx) * (y[i] - my);
    den += (i - mx) * (i - mx);
  }
  return den > 0.f ? num / den : 0.f;
}
}  // namespace

const char* periodClassName(PeriodClass c) { return kPeriodNames[static_cast<int>(c)]; }
const char* modelStatusName(ModelStatus s) { return kStatusNames[static_cast<int>(s)]; }

ThermalModel::ThermalModel(const ModelConfig* cfg) : cfg_(cfg) { reset(); }

void ThermalModel::reset() {
  theta_ = {1.0f, 0.05f, 0.f};  // weak prior: 1 K/h heating gain
  for (int i = 0; i < kParams; ++i)
    for (int j = 0; j < kParams; ++j) P_[i][j] = i == j ? kPInit : 0.f;
  sigma2_ = 0.04f;
  baselineSigma2_ = 0.f;
  counts_.fill(0);
  status_ = ModelStatus::Learning;
  lastClass_ = PeriodClass::Incomplete;
  minutes_ = 0;
  haveStart_ = false;
  tSum_ = outSum_ = 0.f;
  tCount_ = outCount_ = 0;
  flagVent_ = flagIncomplete_ = flagManual_ = flagFault_ = false;
  dutyHist_.clear();
  trend_ = 0.f;
  haveLastTemp_ = false;
  delayTracking_ = false;
  delayMean_ = 30.f;
  delayM2_ = 0.f;
  delayCount_ = 0;
  poorResponseCount_ = 0;
  faultSuspected_ = false;
  temps_.clear();
}

void ThermalModel::noteContext(const char* what, Millis now, int64_t epoch) {
  if (context_.size() >= 16) context_.pop_front();
  context_.push_back({epoch, now, what ? what : ""});
}

float ThermalModel::paramStd(int i) const { return std::sqrt(std::max(0.f, P_[i][i]) * sigma2_); }
float ThermalModel::residualStd() const { return std::sqrt(sigma2_); }
float ThermalModel::delayStdMin() const {
  if (delayCount_ < 2) return 30.f;
  return std::sqrt(delayM2_ / (delayCount_ - 1));
}

float ThermalModel::delayedDuty(float delayMin, float spanMin) const {
  // average duty over [now - delay - span, now - delay]
  if (dutyHist_.empty()) return 0.f;
  int d = static_cast<int>(delayMin + 0.5f);
  int s = std::max(1, static_cast<int>(spanMin + 0.5f));
  float sum = 0.f;
  int n = 0;
  for (int k = d; k < d + s; ++k) {
    int idx = static_cast<int>(dutyHist_.size()) - 1 - k;
    if (idx < 0) break;
    sum += dutyHist_[static_cast<size_t>(idx)];
    ++n;
  }
  return n ? sum / n : 0.f;
}

void ThermalModel::trackDelay(const ThermalInput& in, Millis now, bool disturbed) {
  bool heating = in.duty > 0.5f;
  bool longOff = !wasHeating_ && (lastHeatOffMs_ == 0 || elapsed(now, lastHeatOffMs_) >= 30u * 60000u);
  if (heating && !wasHeating_ && longOff && in.tempValid && !disturbed) {
    delayTracking_ = true;
    heatStartMs_ = now;
    tMin_ = in.temp;
    tMinMs_ = now;
  }
  if (delayTracking_) {
    if (disturbed || !in.tempValid || !heating || elapsed(now, heatStartMs_) > 4u * 3600u * 1000u) {
      delayTracking_ = false;
    } else {
      if (in.temp < tMin_) {
        tMin_ = in.temp;
        tMinMs_ = now;
      }
      if (in.temp >= tMin_ + 0.2f) {
        float d = elapsed(tMinMs_, heatStartMs_) / 60000.f;
        // Welford with a capped count keeps the estimate adaptive
        if (delayCount_ < 20) ++delayCount_;
        float delta = d - delayMean_;
        delayMean_ += delta / delayCount_;
        delayM2_ += delta * (d - delayMean_);
        if (delayCount_ >= 20) delayM2_ *= 0.95f;
        delayMean_ = clampf(delayMean_, 0.f, 180.f);
        delayTracking_ = false;
      }
    }
  }
  if (!heating && wasHeating_) lastHeatOffMs_ = now;
  wasHeating_ = heating;
}

bool ThermalModel::observe(const ThermalInput& in, Millis now) {
  const ModelConfig& c = cfg_ ? *cfg_ : defaultCfg_;
  dutyHist_.push_back(clampf(in.duty, 0.f, 1.f));
  if (dutyHist_.size() > kDutyHistory) dutyHist_.pop_front();

  if (in.tempValid && !in.tempFallback) {
    if (haveLastTemp_) trend_ += 0.1f * ((in.temp - lastTemp_) * 60.f - trend_);
    lastTemp_ = in.temp;
    haveLastTemp_ = true;
  }
  if (in.windowOpen) {
    flagVent_ = true;
    lastWindowMs_ = now;
    windowSeen_ = true;
  } else if (windowSeen_ && elapsed(now, lastWindowMs_) < c.ventilationRecoveryMin * 60000u) {
    flagVent_ = true;
  }
  if (!in.tempValid || in.tempFallback) flagIncomplete_ = true;
  if (in.manualOutput || (in.manualSetpoint && !c.learnDuringManualSetpoint)) flagManual_ = true;
  if (in.fault) flagFault_ = true;
  if (in.outdoorValid) {
    outSum_ += in.outdoor;
    ++outCount_;
    lastOutdoor_ = in.outdoor;
    haveOutdoor_ = true;
  }
  if (in.tempValid && !in.tempFallback) {
    temps_.push_back(in.temp);
    tSum_ += in.temp;
    ++tCount_;
    haveStart_ = true;
  }
  trackDelay(in, now, in.windowOpen || flagFault_ || flagManual_);
  if (++minutes_ >= c.sampleMin) {
    finishInterval(now);
    return true;
  }
  return false;
}

void ThermalModel::finishInterval(Millis now) {
  (void)now;
  const ModelConfig& c = cfg_ ? *cfg_ : defaultCfg_;
  PeriodClass cls = PeriodClass::Normal;
  if (tCount_ < c.sampleMin) flagIncomplete_ = true;
  if (flagFault_) cls = PeriodClass::HeatingFault;
  else if (flagVent_) cls = PeriodClass::Ventilation;
  else if (flagIncomplete_) cls = PeriodClass::Incomplete;
  else if (flagManual_) cls = PeriodClass::Manual;

  if (cls == PeriodClass::Normal && temps_.size() >= 3) {
    float y = slope(temps_) * 60.f;  // K/h
    float tMean = tSum_ / tCount_;
    float tref = outCount_ ? outSum_ / outCount_ : 0.f;
    float u = delayedDuty(delayMean_, c.sampleMin);
    float phi[kParams] = {u, tref - tMean, 1.f};
    float pred = theta_[0] * phi[0] + theta_[1] * phi[1] + theta_[2] * phi[2];
    // Plausibility of the heating response: full duty but clearly less
    // warming than the trusted model predicts -> suspected heating fault.
    if (status_ == ModelStatus::Ready && u > 0.8f && pred - y > std::max(3.f * residualStd(), 0.3f)) {
      if (++poorResponseCount_ >= 6) faultSuspected_ = true;
      cls = PeriodClass::HeatingFault;
    } else {
      if (poorResponseCount_ > 0) --poorResponseCount_;
      if (poorResponseCount_ == 0) faultSuspected_ = false;
      rlsUpdate(phi, y);
    }
  }
  ++counts_[static_cast<size_t>(cls)];
  lastClass_ = cls;
  minutes_ = 0;
  haveStart_ = false;
  tSum_ = outSum_ = 0.f;
  tCount_ = outCount_ = 0;
  temps_.clear();
  flagVent_ = flagIncomplete_ = flagManual_ = flagFault_ = false;
  updateStatus();
}

void ThermalModel::rlsUpdate(const float phi[kParams], float y) {
  const ModelConfig& c = cfg_ ? *cfg_ : defaultCfg_;
  float lambda = clampf(c.forgetting, 0.95f, 1.f);
  float Pphi[kParams];
  for (int i = 0; i < kParams; ++i) {
    Pphi[i] = 0.f;
    for (int j = 0; j < kParams; ++j) Pphi[i] += P_[i][j] * phi[j];
  }
  float denom = lambda;
  for (int i = 0; i < kParams; ++i) denom += phi[i] * Pphi[i];
  if (denom <= 1e-6f) return;
  float e = y;
  for (int i = 0; i < kParams; ++i) e -= theta_[i] * phi[i];
  float K[kParams];
  for (int i = 0; i < kParams; ++i) K[i] = Pphi[i] / denom;
  for (int i = 0; i < kParams; ++i) theta_[i] += K[i] * e;
  for (int i = 0; i < kParams; ++i)
    for (int j = 0; j < kParams; ++j) P_[i][j] = (P_[i][j] - K[i] * Pphi[j]) / lambda;
  // symmetrise and bound (prevents covariance wind-up without excitation)
  for (int i = 0; i < kParams; ++i) {
    for (int j = i + 1; j < kParams; ++j) P_[i][j] = P_[j][i] = 0.5f * (P_[i][j] + P_[j][i]);
    if (P_[i][i] > kPMax) {
      float s = kPMax / P_[i][i];
      for (int j = 0; j < kParams; ++j) {
        P_[i][j] *= std::sqrt(s);
        P_[j][i] *= std::sqrt(s);
      }
    }
    if (P_[i][i] < 1e-6f) P_[i][i] = 1e-6f;
  }
  theta_[0] = clampf(theta_[0], 0.f, 15.f);
  theta_[1] = clampf(theta_[1], 0.f, 2.f);
  theta_[2] = clampf(theta_[2], -10.f, 10.f);
  sigma2_ = 0.98f * sigma2_ + 0.02f * e * e;
  if (sigma2_ < 1e-4f) sigma2_ = 1e-4f;
}

void ThermalModel::updateStatus() {
  const ModelConfig& c = cfg_ ? *cfg_ : defaultCfg_;
  if (status_ == ModelStatus::Disabled) return;
  uint32_t normal = counts_[static_cast<size_t>(PeriodClass::Normal)];
  bool confident = normal >= c.minSamples && theta_[0] > 0.05f && paramStd(0) / theta_[0] < c.maxRelStdA;
  if (faultSuspected_) {
    status_ = ModelStatus::Degraded;
  } else if (confident) {
    if (status_ != ModelStatus::Ready) {
      if (baselineSigma2_ <= 0.f || status_ == ModelStatus::Learning) baselineSigma2_ = sigma2_;
    }
    status_ = baselineSigma2_ > 0.f && sigma2_ > 4.f * baselineSigma2_ ? ModelStatus::Degraded : ModelStatus::Ready;
  } else {
    status_ = normal >= c.minSamples ? ModelStatus::Degraded : ModelStatus::Learning;
  }
}

Prediction ThermalModel::predict(float horizonMin, float futureDuty) const {
  Prediction p;
  if (!haveLastTemp_) return p;
  const ModelConfig& c = cfg_ ? *cfg_ : defaultCfg_;
  int H = static_cast<int>(clampf(horizonMin, 1.f, 240.f));
  int d = static_cast<int>(delayMean_ + 0.5f);
  float T = lastTemp_;
  float tref = haveOutdoor_ ? lastOutdoor_ : 0.f;
  float sdParam = 0.f;
  for (int k = 0; k < H; ++k) {
    float u;
    int back = d - k;  // input applied now was commanded `back` minutes ago
    if (back > 0) {
      int idx = static_cast<int>(dutyHist_.size()) - back;
      u = idx >= 0 ? dutyHist_[static_cast<size_t>(idx)] : 0.f;
    } else {
      u = clampf(futureDuty, 0.f, 1.f);
    }
    float phi[kParams] = {u, tref - T, 1.f};
    float dTdt = theta_[0] * phi[0] + theta_[1] * phi[1] + theta_[2] * phi[2];
    T += dTdt / 60.f;
    float q = 0.f;
    for (int i = 0; i < kParams; ++i)
      for (int j = 0; j < kParams; ++j) q += phi[i] * P_[i][j] * phi[j];
    sdParam += std::sqrt(std::max(0.f, q) * sigma2_) / 60.f;  // correlated parameter error
  }
  float steps = static_cast<float>(H) / c.sampleMin;
  float sdNoise = std::sqrt(sigma2_) * (c.sampleMin / 60.f) * std::sqrt(steps) * 0.5f;
  p.valid = status_ == ModelStatus::Ready;
  p.mean = T;
  p.stddev = sdParam + sdNoise;
  return p;
}

ModelAdvice ThermalModel::advice(bool heatingOn, float currentTemp) const {
  ModelAdvice a;
  const ModelConfig& c = cfg_ ? *cfg_ : defaultCfg_;
  if (status_ != ModelStatus::Ready || delayCount_ == 0) return a;
  // Switching now only takes effect after the dead time: look at where the
  // temperature goes during that time with the heating switched off.
  Prediction p = predict(std::max(5.f, delayMean_), 0.f);
  if (!p.valid || p.stddev > c.maxAdviceStd) return a;
  a.valid = true;
  float delta = p.mean - currentTemp;
  if (heatingOn) a.stopEarlyK = clampf(delta, 0.f, c.maxAdviceK);
  else a.startEarlyK = clampf(-delta, 0.f, c.maxAdviceK);
  return a;
}

void ThermalModel::toJson(JsonObject o, bool runtime) const {
  JsonArray th = o["theta"].to<JsonArray>();
  for (float v : theta_) th.add(v);
  JsonArray pa = o["P"].to<JsonArray>();
  for (auto& row : P_)
    for (float v : row) pa.add(v);
  o["sigma2"] = sigma2_;
  o["baselineSigma2"] = baselineSigma2_;
  JsonArray ca = o["counts"].to<JsonArray>();
  for (auto v : counts_) ca.add(v);
  o["delayMean"] = delayMean_;
  o["delayM2"] = delayM2_;
  o["delayCount"] = delayCount_;
  o["disabled"] = status_ == ModelStatus::Disabled;
  if (!runtime) return;
  o["status"] = modelStatusName(status_);
  o["heatingGainKh"] = theta_[0];
  o["heatingGainStd"] = paramStd(0);
  o["lossCoeff"] = theta_[1];
  o["lossCoeffStd"] = paramStd(1);
  o["offsetKh"] = theta_[2];
  o["residualStd"] = residualStd();
  o["trendKh"] = trend_;
  o["delayMin"] = delayMean_;
  o["delayStdMin"] = delayStdMin();
  o["lastClass"] = periodClassName(lastClass_);
  o["faultSuspected"] = faultSuspected_;
  JsonObject cls = o["periods"].to<JsonObject>();
  for (size_t i = 0; i < counts_.size(); ++i) cls[kPeriodNames[i]] = counts_[i];
  Prediction p = predict(60.f, dutyHist_.empty() ? 0.f : dutyHist_.back());
  if (haveLastTemp_) {
    JsonObject pr = o["prediction60"].to<JsonObject>();
    pr["valid"] = p.valid;
    pr["mean"] = p.mean;
    pr["std"] = p.stddev;
  }
  JsonArray ctx = o["context"].to<JsonArray>();
  for (auto& e : context_) {
    JsonObject x = ctx.add<JsonObject>();
    x["epoch"] = e.epoch;
    x["what"] = e.what;
  }
}

void ThermalModel::fromJson(JsonObjectConst o) {
  reset();
  JsonArrayConst th = o["theta"];
  if (th.size() == kParams)
    for (int i = 0; i < kParams; ++i) theta_[i] = th[i] | theta_[i];
  JsonArrayConst pa = o["P"];
  if (pa.size() == kParams * kParams)
    for (int i = 0; i < kParams; ++i)
      for (int j = 0; j < kParams; ++j) P_[i][j] = pa[i * kParams + j] | P_[i][j];
  sigma2_ = o["sigma2"] | sigma2_;
  baselineSigma2_ = o["baselineSigma2"] | 0.f;
  JsonArrayConst ca = o["counts"];
  for (size_t i = 0; i < counts_.size() && i < ca.size(); ++i) counts_[i] = ca[i] | 0u;
  delayMean_ = o["delayMean"] | 30.f;
  delayM2_ = o["delayM2"] | 0.f;
  delayCount_ = o["delayCount"] | 0u;
  if (o["disabled"] | false) status_ = ModelStatus::Disabled;
  else updateStatus();
}

// ------------------------------------------------------------- manager

ThermalModel& ThermalModelManager::model(const std::string& room) {
  for (auto& m : models_)
    if (m.first == room) return m.second;
  models_.emplace_back(room, ThermalModel(&cfg));
  return models_.back().second;
}

const ThermalModel* ThermalModelManager::find(const std::string& room) const {
  for (auto& m : models_)
    if (m.first == room) return &m.second;
  return nullptr;
}

void ThermalModelManager::removeMissing(const std::vector<Room>& rooms) {
  models_.erase(std::remove_if(models_.begin(), models_.end(),
                               [&](const std::pair<std::string, ThermalModel>& m) {
                                 for (auto& r : rooms)
                                   if (r.id == m.first) return false;
                                 return true;
                               }),
                models_.end());
}

void ThermalModelManager::observe(const std::string& room, const ThermalInput& in, Millis now) {
  if (!enabled) return;
  ThermalModel& m = model(room);
  ModelStatus before = m.status();
  bool faultBefore = m.faultSuspected();
  if (!m.observe(in, now)) return;
  const char* what = nullptr;
  if (m.faultSuspected() && !faultBefore) what = "fault_suspected";
  else if (m.status() != before) what = modelStatusName(m.status());
  if (what) {
    Event e(EventType::ModelEvent, EventSource::Model);
    e.ts = now;
    e.setRef(room.c_str());
    e.setText(what);
    bus_.post(e);
  }
}

void ThermalModelManager::noteContext(const std::string& room, const char* what) {
  model(room).noteContext(what, clock_.nowMs(), clock_.epoch());
}

ModelAdvice ThermalModelManager::advice(const std::string& room, bool heatingOn, float currentTemp) const {
  if (!enabled) return ModelAdvice{};
  const ThermalModel* m = find(room);
  return m ? m->advice(heatingOn, currentTemp) : ModelAdvice{};
}

void ThermalModelManager::reset(const std::string& room) { model(room).reset(); }

void ThermalModelManager::toJson(JsonObject o, bool runtime) const {
  o["enabled"] = enabled;
  JsonObject c = o["config"].to<JsonObject>();
  c["sampleMin"] = cfg.sampleMin;
  c["forgetting"] = cfg.forgetting;
  c["minSamples"] = cfg.minSamples;
  c["ventilationRecoveryMin"] = cfg.ventilationRecoveryMin;
  c["learnDuringManualSetpoint"] = cfg.learnDuringManualSetpoint;
  c["maxAdviceK"] = cfg.maxAdviceK;
  JsonObject ms = o["models"].to<JsonObject>();
  for (auto& m : models_) m.second.toJson(ms[m.first].to<JsonObject>(), runtime);
}

void ThermalModelManager::fromJson(JsonObjectConst o) {
  enabled = o["enabled"] | true;
  JsonObjectConst c = o["config"];
  cfg.sampleMin = clampf(c["sampleMin"] | 10, 5, 30);
  cfg.forgetting = clampf(c["forgetting"] | 0.995f, 0.95f, 1.f);
  cfg.minSamples = c["minSamples"] | 48;
  cfg.ventilationRecoveryMin = c["ventilationRecoveryMin"] | 30;
  cfg.learnDuringManualSetpoint = c["learnDuringManualSetpoint"] | false;
  cfg.maxAdviceK = clampf(c["maxAdviceK"] | 0.5f, 0.f, 0.5f);
  models_.clear();
  for (JsonPairConst kv : o["models"].as<JsonObjectConst>()) {
    ThermalModel m(&cfg);
    m.fromJson(kv.value().as<JsonObjectConst>());
    models_.emplace_back(kv.key().c_str(), m);
  }
}

}  // namespace hc
