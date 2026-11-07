// PWM (time proportional PI) room control.
#include "SimRig.h"

using namespace hc;
using sim::Rig;

namespace {

void pwmRoom(Rig& r, const char* extra = "", int minSec = 60) {
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                R"({"rooms":[{"id":"wohnen","name":"Wohnen","comfort":21,"eco":18,"frost":7,"mode":"comfort",
                  "hysteresis":0.4,"control":"pwm","pwmCycleMin":20%s}],
                  "circuits":[{"n":1,"room":"wohnen","minOnSec":%d,"minOffSec":%d}]})",
                extra, minSec, minSec);
  r.load(ConfigSection::Heating, buf);
  r.load(ConfigSection::Sensors, R"({"sensors":[{"id":"ow1:01","name":"F","room":"wohnen","timeoutSec":300}]})");
}

// holds the room temperature constant; returns seconds the output was on
uint32_t holdTemp(Rig& r, float t, uint32_t sec) {
  uint32_t on = 0;
  for (uint32_t s = 0; s < sec; ++s) {
    if (s % 30 == 0) r.sys.sensors.update("ow1:01", t, r.clock.ms);
    r.run(1000, false);
    if (r.output(1)) ++on;
  }
  return on;
}

}  // namespace

TEST(pwm_config_roundtrip_and_validation) {
  Rig r;
  pwmRoom(r, R"(,"pwmKp":55,"pwmTnMin":120)");
  const Room* w = r.sys.heating.room("wohnen");
  CHECK(w->control == RoomControl::Pwm);
  CHECK_EQ(w->pwmCycleMin, 20);
  CHECK_NEAR(w->pwmKp, 55.f, 0.01f);
  CHECK_EQ(w->pwmTnMin, 120);
  JsonDocument d;
  r.sys.sectionToJson(ConfigSection::Heating, d.to<JsonObject>(), false);
  CHECK_STR(std::string(d["rooms"][0]["control"] | ""), "pwm");
  CHECK_EQ(d["rooms"][0]["pwmTnMin"].as<int>(), 120);

  // defaults: hysteresis controller
  r.basicRoom();
  CHECK(r.sys.heating.room("wohnen")->control == RoomControl::Hysteresis);

  auto bad = [&](const char* json) {
    JsonDocument x;
    deserializeJson(x, json);
    return r.sys.sectionFromJson(ConfigSection::Heating, x.as<JsonObjectConst>());
  };
  CHECK(!bad(R"({"rooms":[{"id":"a","control":"pid"}]})").empty());
  CHECK(!bad(R"({"rooms":[{"id":"a","control":"pwm","pwmCycleMin":2}]})").empty());
  CHECK(!bad(R"({"rooms":[{"id":"a","control":"pwm","pwmKp":0}]})").empty());
  CHECK(!bad(R"({"rooms":[{"id":"a","control":"pwm","pwmTnMin":601}]})").empty());
  CHECK(bad(R"({"rooms":[{"id":"a","control":"pwm","pwmTnMin":0}]})").empty());
  // cycle must hold a minimum pulse plus a minimum pause (2 x 180 s > 5 min)
  std::string e = bad(R"({"rooms":[{"id":"a","control":"pwm","pwmCycleMin":5}],
                         "circuits":[{"n":3,"room":"a","minOnSec":180,"minOffSec":120}]})");
  CHECK(e.find("PWM-Zyklus zu kurz") != std::string::npos);
  CHECK_EQ(r.sys.heating.circuits[2].minOnSec, 120);  // rejected config left nothing behind
  CHECK(bad(R"({"rooms":[{"id":"a","control":"pwm","pwmCycleMin":6}],
               "circuits":[{"n":3,"room":"a","minOnSec":180,"minOffSec":120}]})").empty());
}

TEST(pwm_proportional_duty_one_pulse_per_cycle) {
  Rig r;
  pwmRoom(r, R"(,"pwmKp":40,"pwmTnMin":0)");
  // 0.5 K below setpoint, P only: 40 %/K * 0.5 K = 20 % of 20 min = 4 min
  uint32_t on = holdTemp(r, 20.5f, 20 * 60);
  CHECK_NEAR(r.sys.heating.room("wohnen")->duty, 20.f, 0.5f);
  CHECK(on >= 230 && on <= 260);
  // one switch-on per cycle, not a chattering output
  uint32_t sw = r.sys.io.out[0].switches;
  holdTemp(r, 20.5f, 40 * 60);
  uint32_t perCycle = (r.sys.io.out[0].switches - sw) / 2;
  CHECK_EQ(perCycle, 2u);
  // above the setpoint: closed
  on = holdTemp(r, 21.2f, 25 * 60);
  CHECK(on < 60);
  CHECK_NEAR(r.sys.heating.room("wohnen")->duty, 0.f, 0.01f);
}

TEST(pwm_short_pulses_respect_actuator_time) {
  Rig r;
  // minimum on/off 180 s (thermal actuator); P only, 40 %/K
  pwmRoom(r, R"(,"pwmKp":40,"pwmTnMin":0)", 180);
  // 0.05 K below: 2 % = 24 s -> too short, no pulse at all
  CHECK_EQ(holdTemp(r, 20.95f, 20 * 60), 0u);
  // 0.2 K below: 8 % = 96 s >= half the minimum -> stretched to 180 s
  uint32_t on = holdTemp(r, 20.8f, 20 * 60);
  CHECK(on >= 175 && on <= 200);
  // 2.4 K below: 96 % -> remaining off time below 90 s -> fully on
  holdTemp(r, 18.6f, 20 * 60);
  on = holdTemp(r, 18.6f, 20 * 60);
  CHECK(on >= 20 * 60 - 15);
}

TEST(pwm_integral_removes_offset_and_holds_setpoint) {
  Rig r;
  pwmRoom(r, R"(,"pwmKp":40,"pwmTnMin":90)");
  r.plants["wohnen"] = sim::Plant{};
  r.plants["wohnen"].T = 19.f;
  r.sensorOf["wohnen"] = "ow1:01";
  r.run(30u * 3600u * 1000u);
  // last 8 h: mean close to the setpoint, small band, cycling not chattering
  float sum = 0.f, lo = 99.f, hi = -99.f;
  uint32_t sw0 = r.sys.io.out[0].switches;
  for (int m = 0; m < 8 * 60; ++m) {
    r.run(60000);
    float t = r.plants["wohnen"].T;
    sum += t;
    lo = std::min(lo, t);
    hi = std::max(hi, t);
  }
  float mean = sum / (8 * 60);
  CHECK_NEAR(mean, 21.f, 0.25f);
  CHECK(hi - lo < 0.8f);
  uint32_t switchesPerHour = (r.sys.io.out[0].switches - sw0) / 8;
  CHECK(switchesPerHour <= 8);  // at most one pulse per 20 min cycle (on + off)
  const Room* w = r.sys.heating.room("wohnen");
  CHECK(w->pwmI > 20.f);  // the plant needs ~57 % duty in steady state
}

TEST(pwm_window_and_overshoot_close_immediately) {
  Rig r;
  pwmRoom(r, R"(,"pwmKp":40,"pwmTnMin":60)");
  holdTemp(r, 19.f, 120);
  CHECK(r.output(1));  // 2 K below: 80 %+ duty, pulse running
  // overshoot guard: reaching setpoint + hysteresis closes within the cycle
  holdTemp(r, 21.5f, 100);
  CHECK(!r.output(1));
  // falling just below the guard does not re-open within the same cycle
  CHECK_EQ(holdTemp(r, 21.3f, 300), 0u);
  holdTemp(r, 19.f, 25 * 60);
  float iBefore = r.sys.heating.room("wohnen")->pwmI;
  CHECK(r.output(1));
  r.sys.heating.setWindow(*r.sys.heating.room("wohnen"), "test", true, r.clock.ms, 0, 0, EventSource::Web);
  holdTemp(r, 19.f, 90);
  CHECK(!r.output(1));
  // the integral does not wind down while the window is open
  holdTemp(r, 19.f, 30 * 60);
  CHECK_NEAR(r.sys.heating.room("wohnen")->pwmI, iBefore, 1.0f);
  r.sys.heating.setWindow(*r.sys.heating.room("wohnen"), "test", false, r.clock.ms, 0, 0, EventSource::Web);
  holdTemp(r, 19.f, 90);
  CHECK(r.output(1));
}

TEST(pwm_status_reports_duty) {
  Rig r;
  pwmRoom(r, R"(,"pwmKp":40,"pwmTnMin":0)");
  holdTemp(r, 20.f, 60);
  JsonDocument d;
  r.sys.heating.toJson(d.to<JsonObject>(), true);
  CHECK_EQ(d["rooms"][0]["duty"].as<int>(), 40);
  CHECK(d["rooms"][0]["pwmCycleRemainingSec"].as<int>() > 0);
}
