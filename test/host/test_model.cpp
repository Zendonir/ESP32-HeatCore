#include <cmath>
#include <random>

#include "SimRig.h"
#include "hc/model/ThermalModel.h"

using namespace hc;

namespace {

struct ModelSim {
  sim::Plant plant;
  ModelConfig cfg;
  ThermalModel informed{&cfg};
  ThermalModel blind{&cfg};  // never told about the window
  bool valve = false;
  Millis now = 0;
  std::mt19937 rng{1};

  void minute(bool windowOpen, bool reportWindow = true) {
    // bang-bang control around 21 C gives enough excitation
    if (plant.T < 20.8f) valve = true;
    if (plant.T > 21.2f) valve = false;
    plant.windowOpen = windowOpen;
    plant.step(valve ? 1.f : 0.f);
    std::normal_distribution<float> noise(0.f, 0.02f);
    float measured = std::round((plant.T + noise(rng)) * 16.f) / 16.f;
    ThermalInput in;
    in.tempValid = true;
    in.temp = measured;
    in.outdoorValid = true;
    in.outdoor = plant.outdoor;
    in.duty = valve ? 1.f : 0.f;
    in.windowOpen = windowOpen && reportWindow;
    now += 60000;
    informed.observe(in, now);
    in.windowOpen = false;
    blind.observe(in, now);
  }
};

}  // namespace

TEST(model_converges_to_plant_parameters) {
  ModelSim s;
  s.plant.T = 19.f;
  for (int m = 0; m < 4 * 24 * 60; ++m) s.minute(false);
  const ThermalModel& m = s.informed;
  CHECK(m.status() == ModelStatus::Ready);
  CHECK_NEAR(m.heatingGain(), 4.0, 0.8);
  CHECK_NEAR(m.lossCoeff(), 0.12, 0.06);
  CHECK(m.delaySamples() > 0);
  CHECK_NEAR(m.delayMin(), 20.0, 12.0);
  CHECK(m.paramStd(0) > 0.f);
  // prediction with uncertainty; compare against the true plant
  Prediction p = m.predict(60.f, 0.f);
  CHECK(p.valid);
  CHECK(p.stddev > 0.f && p.stddev < 2.f);
  sim::Plant copy = s.plant;
  for (int i = 0; i < 60; ++i) copy.step(0.f);
  CHECK_NEAR(p.mean, copy.T, 0.6);
  ModelAdvice a = m.advice(true, s.plant.T);
  if (a.valid) CHECK(a.stopEarlyK >= 0.f && a.stopEarlyK <= 0.5f);
}

TEST(model_excludes_ventilation_periods) {
  ModelSim s;
  s.plant.T = 19.f;
  for (int m = 0; m < 5 * 24 * 60; ++m) {
    bool window = (m % 240) < 40;  // window open 40 min every 4 h
    s.minute(window);
  }
  CHECK(s.informed.samples(PeriodClass::Ventilation) > 50);
  CHECK_EQ(s.blind.samples(PeriodClass::Ventilation), 0u);
  float errInformed = std::fabs(s.informed.lossCoeff() - 0.12f) + std::fabs(s.informed.heatingGain() - 4.f) / 10.f;
  float errBlind = std::fabs(s.blind.lossCoeff() - 0.12f) + std::fabs(s.blind.heatingGain() - 4.f) / 10.f;
  CHECK(errInformed < errBlind);
  CHECK_NEAR(s.informed.lossCoeff(), 0.12, 0.07);
  // an open window is never a heating fault
  CHECK(!s.informed.faultSuspected());
}

TEST(model_detects_missing_heating_effect) {
  ModelSim s;
  s.plant.T = 19.f;
  for (int m = 0; m < 4 * 24 * 60; ++m) s.minute(false);
  CHECK(s.informed.status() == ModelStatus::Ready);
  s.plant.valveBroken = true;
  for (int m = 0; m < 4 * 60; ++m) s.minute(false);
  CHECK(s.informed.faultSuspected());
  CHECK(s.informed.status() == ModelStatus::Degraded);
  CHECK(s.informed.samples(PeriodClass::HeatingFault) > 0);
}

TEST(model_incomplete_and_manual_periods_not_learned) {
  ModelConfig cfg;
  ThermalModel m(&cfg);
  Millis now = 0;
  for (int i = 0; i < 200; ++i) {
    ThermalInput in;
    in.tempValid = i % 3 != 0;  // gaps
    in.temp = 20.f;
    in.duty = 1.f;
    now += 60000;
    m.observe(in, now);
  }
  CHECK(m.samples(PeriodClass::Incomplete) >= 19);
  CHECK_EQ(m.samples(PeriodClass::Normal), 0u);
  for (int i = 0; i < 100; ++i) {
    ThermalInput in;
    in.tempValid = true;
    in.temp = 20.f;
    in.duty = 1.f;
    in.manualOutput = true;
    now += 60000;
    m.observe(in, now);
  }
  CHECK(m.samples(PeriodClass::Manual) >= 9);
  CHECK_EQ(m.samples(PeriodClass::Normal), 0u);
  CHECK(m.status() == ModelStatus::Learning);
  CHECK(!m.advice(true, 20.f).valid);  // fallback: no advice while learning
}

TEST(model_persistence_roundtrip) {
  ModelSim s;
  s.plant.T = 19.f;
  for (int m = 0; m < 3 * 24 * 60; ++m) s.minute(false);
  JsonDocument d;
  s.informed.toJson(d.to<JsonObject>(), false);
  ThermalModel r(&s.cfg);
  r.fromJson(d.as<JsonObjectConst>());
  CHECK_NEAR(r.heatingGain(), s.informed.heatingGain(), 1e-5);
  CHECK_NEAR(r.lossCoeff(), s.informed.lossCoeff(), 1e-5);
  CHECK(r.status() == s.informed.status());
  CHECK_EQ(r.samples(PeriodClass::Normal), s.informed.samples(PeriodClass::Normal));
}

TEST(model_window_event_in_full_system_classified) {
  sim::Rig r;
  r.basicRoom();
  r.load(ConfigSection::Rf, R"({"rx":[{"id":1,"name":"Fenster","function":"window","room":"wohnen",
     "buttons":[{"index":1,"name":"auf","code":"0xA1B2C3","role":"state_on"},{"index":2,"name":"zu","code":"0xA1B2C4","role":"state_off"}]}]})");
  r.plants["wohnen"].T = 19.f;
  r.run(30 * 60 * 1000);
  r.sendRf(0xA1B2C3);
  r.plants["wohnen"].windowOpen = true;
  r.run(30 * 60 * 1000);
  r.sendRf(0xA1B2C4);
  r.plants["wohnen"].windowOpen = false;
  r.run(10 * 60 * 1000);
  const ThermalModel* m = r.sys.model.find("wohnen");
  CHECK(m != nullptr);
  if (m) {
    CHECK(m->samples(PeriodClass::Ventilation) >= 3);
    CHECK(m->samples(PeriodClass::Normal) >= 1);
    bool ctx = false;
    for (auto& c : m->context()) ctx |= c.what == "Fenster geoeffnet";
    CHECK(ctx);
  }
}
