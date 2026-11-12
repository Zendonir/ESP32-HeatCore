// Rooms with several heating circuits: one sensor, the heat split by share.
#include "SimRig.h"

using namespace hc;
using sim::Rig;

namespace {
// returns open seconds of circuits 1 and 2 over `sec` seconds at room temperature t
std::pair<uint32_t, uint32_t> openTime(Rig& r, float t, uint32_t sec) {
  uint32_t a = 0, b = 0;
  for (uint32_t s = 0; s < sec; ++s) {
    if (s % 30 == 0) r.sys.sensors.update("ow1:01", t, r.clock.ms);
    r.run(1000, false);
    if (r.output(1)) ++a;
    if (r.output(2)) ++b;
  }
  return {a, b};
}
}  // namespace

TEST(circuit_share_two_point_room) {
  Rig r;
  r.load(ConfigSection::Heating, R"({"rooms":[{"id":"wohnen","name":"Wohnen","comfort":21,"eco":18,"frost":7,"mode":"comfort",
      "pwmCycleMin":20}],
      "circuits":[{"n":1,"room":"wohnen","minOnSec":60,"minOffSec":60},
                  {"n":2,"room":"wohnen","minOnSec":60,"minOffSec":60,"share":40}]})");
  r.load(ConfigSection::Sensors, R"({"sensors":[{"id":"ow1:01","name":"F","room":"wohnen","timeoutSec":300}]})");
  openTime(r, 18.f, 200);  // start-up, demand begins
  auto o = openTime(r, 18.f, 3 * 20 * 60);  // cold room: heats for three cycles
  CHECK(o.first > 3 * 20 * 60 - 30);                       // circuit 1: whole time
  CHECK(o.second > 3 * 8 * 60 - 90 && o.second < 3 * 8 * 60 + 90);  // circuit 2: 40 % = 8 of 20 min
  // the room is warm: both closed
  o = openTime(r, 23.f, 300);
  CHECK(!r.output(1) && !r.output(2));
}

TEST(circuit_share_pwm_room_and_config) {
  Rig r;
  r.load(ConfigSection::Heating, R"({"rooms":[{"id":"wohnen","name":"Wohnen","comfort":21,"eco":18,"frost":7,"mode":"comfort",
      "control":"pwm","pwmCycleMin":20,"pwmKp":40,"pwmTnMin":0}],
      "circuits":[{"n":1,"room":"wohnen","minOnSec":60,"minOffSec":60},
                  {"n":2,"room":"wohnen","minOnSec":60,"minOffSec":60,"share":50}]})");
  r.load(ConfigSection::Sensors, R"({"sensors":[{"id":"ow1:01","name":"F","room":"wohnen","timeoutSec":300}]})");
  openTime(r, 20.f, 25 * 60);
  auto o = openTime(r, 20.f, 3 * 20 * 60);
  CHECK(o.first > 0);
  // circuit 2 gets half of the room's on-time
  CHECK(o.second * 2 > o.first - 180 && o.second * 2 < o.first + 180);
  // stored, kept by pages that do not send it, validated
  JsonDocument cfg;
  r.sys.sectionToJson(ConfigSection::Heating, cfg.to<JsonObject>(), false);
  CHECK_EQ(cfg["circuits"][1]["share"] | 0, 50);
  std::string s;
  cfg["circuits"][1].remove("share");
  serializeJson(cfg, s);
  CHECK_STR(r.sys.sectionFromJson(ConfigSection::Heating, cfg.as<JsonObjectConst>()), "");
  CHECK_EQ(r.sys.heating.circuits[1].share, 50);
  cfg["circuits"][1]["share"] = 150;
  CHECK(!r.sys.sectionFromJson(ConfigSection::Heating, cfg.as<JsonObjectConst>()).empty());
}

TEST(circuit_share_pwm_with_external_demand) {
  // an external heat request keeps a PWM room on for the whole cycle: the
  // circuit gets its share of the whole cycle in one pulse per cycle
  Rig r;
  r.load(ConfigSection::Heating, R"({"rooms":[{"id":"wohnen","name":"Wohnen","comfort":21,"eco":18,"frost":7,"mode":"comfort",
      "control":"pwm","pwmCycleMin":20,"pwmKp":40,"pwmTnMin":0}],
      "circuits":[{"n":1,"room":"wohnen","minOnSec":60,"minOffSec":60},
                  {"n":2,"room":"wohnen","minOnSec":60,"minOffSec":60,"share":50}]})");
  r.load(ConfigSection::Sensors, R"({"sensors":[{"id":"ow1:01","name":"F","room":"wohnen","timeoutSec":300}]})");
  openTime(r, 20.5f, 25 * 60);
  Room* w = r.sys.heating.room("wohnen");
  r.sys.heating.setExternalDemand(*w, "test", true, 4 * 3600, r.clock.ms);
  openTime(r, 20.5f, 20 * 60);  // into the next full cycle
  uint32_t open2 = 0, pulses = 0;
  bool was = r.output(2);
  for (uint32_t s = 0; s < 3 * 20 * 60; ++s) {
    if (s % 30 == 0) r.sys.sensors.update("ow1:01", 20.5f, r.clock.ms);
    r.run(1000, false);
    bool on = r.output(2);
    if (on) ++open2;
    if (on && !was) ++pulses;
    was = on;
  }
  CHECK(open2 > 3 * 10 * 60 - 120 && open2 < 3 * 10 * 60 + 120);  // half of 20 min per cycle
  CHECK(pulses <= 3);  // one pulse per cycle, no second short one
}
