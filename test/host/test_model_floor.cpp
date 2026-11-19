// Learning model on an underfloor heating: the screed stores heat for hours,
// the room reacts to the valve only through that slow store.
#include <cmath>
#include <random>

#include "SimRig.h"
#include "hc/model/ThermalModel.h"

using namespace hc;

namespace {

// slab S is heated by the water, the room T by the slab
struct FloorPlant {
  float S = 21.f, T = 19.f;
  float slabGain = 2.0f;    // K/h of slab temperature rise at full valve
  float slabTau = 3.0f;     // h, slab -> room coupling time constant
  float roomCoupling = 0.25f;  // 1/h
  float loss = 0.05f;       // 1/h to outdoor
  float outdoor = 2.f;
  void step(float valve) {
    float dS = slabGain * valve - (S - T) / slabTau;
    float dT = roomCoupling * (S - T) - loss * (T - outdoor);
    S += dS / 60.f;
    T += dT / 60.f;
  }
};

struct FloorSim {
  FloorPlant plant;
  ModelConfig cfg;
  ThermalModel m{&cfg};
  bool valve = false;
  Millis now = 0;
  int minute = 0;
  std::mt19937 rng{7};
  float lastMeasured = 0.f;

  void step() {
    // schedule: 21 degC 06-22 h, 18 degC at night; two-point control
    int mod = minute % 1440;
    float sp = (mod >= 360 && mod < 1320) ? 21.f : 18.f;
    if (plant.T < sp - 0.2f) valve = true;
    if (plant.T > sp + 0.2f) valve = false;
    plant.outdoor = 2.f + 4.f * std::sin(minute / 1440.f * 6.2832f);
    plant.step(valve ? 1.f : 0.f);
    std::normal_distribution<float> noise(0.f, 0.02f);
    lastMeasured = std::round((plant.T + noise(rng)) * 16.f) / 16.f;
    ThermalInput in;
    in.tempValid = true;
    in.temp = lastMeasured;
    in.outdoorValid = true;
    in.outdoor = plant.outdoor;
    in.duty = valve ? 1.f : 0.f;
    now += 60000;
    ++minute;
    m.observe(in, now);
  }
};

}  // namespace

TEST(model_floor_heating_learns_slow_response) {
  FloorSim s;
  for (int i = 0; i < 6 * 24 * 60; ++i) s.step();
  const ThermalModel& m = s.m;
  // true response of the slab-room difference D = S - T at constant room
  // temperature: dD/dt = slabGain*u - (1/slabTau + roomCoupling)*D, i.e. a lag
  // of 1/(1/3 + 0.25) h = 103 min and a heat input to the room of
  // roomCoupling * slabGain / (1/slabTau + roomCoupling) = 0.86 K/h at full valve
  float trueGain = 0.25f * 2.0f / (1.f / 3.f + 0.25f);
  CHECK(m.status() == ModelStatus::Ready);
  CHECK_NEAR(m.heatingGain(), trueGain, 0.25);
  CHECK_NEAR(m.lossCoeff(), 0.05, 0.04);
  CHECK(m.slowEmitter());  // underfloor response recognised
  CHECK(m.delayMin() >= 60.f && m.delayMin() <= 150.f);
  // prediction one hour ahead with the valve closed: compare with the plant
  Prediction p = m.predict(60.f, 0.f);
  CHECK(p.valid);
  FloorPlant copy = s.plant;
  for (int i = 0; i < 60; ++i) copy.step(0.f);
  CHECK_NEAR(p.mean, copy.T, 0.3);
  // the screed keeps heating after the valve closes: while heating, the
  // model advises to stop early (bounded)
  ModelAdvice a = m.advice(true, s.lastMeasured);
  CHECK(a.valid);
  CHECK(a.stopEarlyK >= 0.f && a.stopEarlyK <= 0.5f);
  // persistence keeps the selected lag model and its stored heat
  JsonDocument d;
  m.toJson(d.to<JsonObject>(), false);
  ThermalModel r(&s.cfg);
  r.fromJson(d.as<JsonObjectConst>());
  CHECK(r.slowEmitter());
  CHECK_NEAR(r.delayMin(), m.delayMin(), 0.01);
  CHECK_NEAR(r.heatingGain(), m.heatingGain(), 1e-5);
  CHECK(r.status() == m.status());
  {  // stored heat restored: after the next observation both predict alike
    ThermalModel live = m;
    ThermalInput in;
    in.tempValid = in.outdoorValid = true;
    in.temp = s.lastMeasured;
    in.outdoor = s.plant.outdoor;
    in.duty = s.valve ? 1.f : 0.f;
    live.observe(in, s.now + 60000);
    r.observe(in, s.now + 60000);
    CHECK_NEAR(r.predict(60.f, 0.f).mean, live.predict(60.f, 0.f).mean, 0.02);
  }
  // a model saved by an earlier version (dead-time model only) still loads
  JsonDocument old;
  deserializeJson(old, R"({"theta":[0.5,0.05,-0.5],"sigma2":0.1,"counts":[60,0,0,0,0],"delayMean":40})");
  ThermalModel o(&s.cfg);
  o.fromJson(old.as<JsonObjectConst>());
  CHECK(!o.slowEmitter());
  CHECK_NEAR(o.heatingGain(), 0.5, 1e-5);
  CHECK_NEAR(o.delayMin(), 40.0, 1e-4);
}

TEST(model_floor_upgrade_from_dead_time_model) {
  // a model learned by an earlier version (dead-time model only, poor fit):
  // the new lag candidates start from scratch and may only take over once they
  // have learned as much as the selection requires
  FloorSim s;
  JsonDocument old;
  deserializeJson(old, R"({"theta":[0.1,0.004,-0.5],"sigma2":0.3,"counts":[483,0,0,0,0],"delayMean":30})");
  s.m.fromJson(old.as<JsonObjectConst>());
  for (int i = 0; i < 6 * 60; ++i) s.step();  // 36 samples
  CHECK(!s.m.slowEmitter());
  {
    JsonDocument d;
    s.m.toJson(d.to<JsonObject>(), true);
    CHECK(d["emitterKnown"] == false);  // the lag candidates have not learned enough yet
  }
  for (int i = 0; i < 5 * 24 * 60; ++i) s.step();
  CHECK(s.m.slowEmitter());
  CHECK(s.m.status() == ModelStatus::Ready);
}
