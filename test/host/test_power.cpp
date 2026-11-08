// Power supply budget for the valve outputs (staggered switch-on).
#include "SimRig.h"

using namespace hc;
using sim::Rig;

namespace {

// six rooms on circuits 1..6, each with its own sensor, all far below setpoint
void sixRooms(Rig& r, bool normallyOpen = false) {
  std::string rooms, circuits, sensors;
  for (int i = 1; i <= 6; ++i) {
    char b[200];
    std::snprintf(b, sizeof(b), R"(%s{"id":"r%d","name":"R%d","comfort":21,"eco":18,"frost":7,"mode":"comfort"})",
                  i > 1 ? "," : "", i, i);
    rooms += b;
    std::snprintf(b, sizeof(b), R"(%s{"n":%d,"room":"r%d","minOnSec":10,"minOffSec":10,"valveNormallyOpen":%s})",
                  i > 1 ? "," : "", i, i, normallyOpen ? "true" : "false");
    circuits += b;
    std::snprintf(b, sizeof(b), R"(%s{"id":"ow1:0%d","name":"F%d","room":"r%d","timeoutSec":600})", i > 1 ? "," : "",
                  i, i, i);
    sensors += b;
  }
  r.load(ConfigSection::Heating, ("{\"rooms\":[" + rooms + "],\"circuits\":[" + circuits + "]}").c_str());
  r.load(ConfigSection::Sensors, ("{\"sensors\":[" + sensors + "]}").c_str());
}

void feed(Rig& r, float t, uint32_t sec, float* maxLoad = nullptr) {
  for (uint32_t s = 0; s < sec; ++s) {
    if (s % 30 == 0)
      for (int i = 1; i <= 6; ++i) r.sys.sensors.update("ow1:0" + std::to_string(i), t, r.clock.ms);
    r.run(1000, false);
    if (maxLoad) *maxLoad = std::max(*maxLoad, r.sys.safety.powerLoadA());
  }
}

int valvesOn(Rig& r) {
  int n = 0;
  for (int i = 1; i <= 6; ++i) n += r.output(i) ? 1 : 0;
  return n;
}

}  // namespace

TEST(power_unlimited_switches_all_at_once) {
  Rig r;
  sixRooms(r);
  feed(r, 18.f, 20);
  CHECK_EQ(valvesOn(r), 6);
  CHECK_EQ(r.sys.safety.powerWaiting(), 0);
}

TEST(power_budget_staggers_valve_start) {
  Rig r;
  sixRooms(r);
  // 24 V / 2.5 A, 80 % usable = 2.0 A; base 0.25 A; Danfoss TWA-A 0.35 A inrush, 2 W
  r.load(ConfigSection::Safety, R"({"psuVoltage":24,"psuCurrentA":2.5,"psuUsablePct":80,"psuBaseLoadA":0.25,
                                   "valveInrushA":0.35,"valveHoldW":2,"valveInrushSec":60})");
  float maxLoad = 0.f;
  feed(r, 18.f, 20, &maxLoad);
  CHECK_EQ(valvesOn(r), 5);  // 0.25 + 5 * 0.35 = 2.0 A
  CHECK_EQ(r.sys.safety.powerWaiting(), 1);
  CHECK(r.sys.io.out[5].blocked.find("Netzteil") != std::string::npos);
  // after the starting phase the first valves draw only 2 W / 24 V
  feed(r, 18.f, 60, &maxLoad);
  CHECK_EQ(valvesOn(r), 6);
  CHECK_EQ(r.sys.safety.powerWaiting(), 0);
  CHECK(maxLoad <= 2.0f + 1e-3f);
  feed(r, 18.f, 120, &maxLoad);
  CHECK_NEAR(r.sys.safety.powerLoadA(), 0.25f + 6 * 2.f / 24.f, 0.01f);
  JsonDocument d;
  r.sys.safety.toJson(d.to<JsonObject>(), true);
  CHECK_NEAR(d["powerBudgetA"].as<float>(), 2.0f, 0.001f);
}

TEST(power_small_supply_never_exceeded) {
  Rig r;
  sixRooms(r);
  // 0.7 A at 100 %: base 0.25 + one start 0.35 fits, steady 2 W valves add 0.083 A each
  r.load(ConfigSection::Safety, R"({"psuCurrentA":0.7,"psuUsablePct":100,"psuBaseLoadA":0.25,
                                   "valveInrushA":0.35,"valveHoldW":2,"valveInrushSec":30})");
  float maxLoad = 0.f;
  feed(r, 18.f, 600, &maxLoad);
  CHECK(maxLoad <= 0.7f + 1e-3f);
  CHECK(valvesOn(r) >= 2);
  CHECK(valvesOn(r) < 6);
  CHECK(r.sys.safety.powerWaiting() > 0);
}

TEST(power_never_delays_the_safe_state) {
  Rig r;
  sixRooms(r, true);  // normally open: energised = closed = safe
  r.load(ConfigSection::Safety, R"({"psuCurrentA":1.0,"psuUsablePct":100,"psuBaseLoadA":0.25,"valveInrushA":0.35,
                                   "valveInrushSec":60,"maxRoomTemp":24})");
  // rooms far above the over-temperature limit: every valve must close now
  feed(r, 28.f, 15);
  CHECK_EQ(valvesOn(r), 6);
}

TEST(power_config_validation) {
  Rig r;
  auto bad = [&](const char* json) {
    JsonDocument x;
    deserializeJson(x, json);
    return r.sys.sectionFromJson(ConfigSection::Safety, x.as<JsonObjectConst>());
  };
  CHECK(!bad(R"({"psuCurrentA":0.5,"psuUsablePct":100,"psuBaseLoadA":0.25,"valveInrushA":0.35})").empty());
  CHECK(!bad(R"({"psuVoltage":48})").empty());
  CHECK(!bad(R"({"psuUsablePct":40})").empty());
  CHECK(!bad(R"({"valveInrushSec":0})").empty());
  CHECK(bad(R"({"psuCurrentA":2.5})").empty());
  CHECK(bad(R"({"psuCurrentA":0})").empty());  // limit off
  JsonDocument d;
  r.sys.sectionToJson(ConfigSection::Safety, d.to<JsonObject>(), false);
  CHECK_NEAR(d["valveHoldW"].as<float>(), 2.f, 0.001f);
}

TEST(power_hold_current_above_inrush_is_reserved) {
  Rig r;
  sixRooms(r);
  // 12 V, 6 W running = 0.5 A > 0.35 A inrush: admission must reserve 0.5 A
  {
    JsonDocument x;
    deserializeJson(x, R"({"psuVoltage":12,"psuCurrentA":0.7,"psuUsablePct":100,"psuBaseLoadA":0.25,
                          "valveInrushA":0.35,"valveHoldW":6})");
    CHECK(!r.sys.sectionFromJson(ConfigSection::Safety, x.as<JsonObjectConst>()).empty());
  }
  r.load(ConfigSection::Safety, R"({"psuVoltage":12,"psuCurrentA":1.3,"psuUsablePct":100,"psuBaseLoadA":0.25,
                                   "valveInrushA":0.35,"valveHoldW":6,"valveInrushSec":20})");
  float maxLoad = 0.f;
  feed(r, 18.f, 120, &maxLoad);
  CHECK_EQ(valvesOn(r), 2);  // 0.25 + 2 * 0.5 = 1.25 A
  CHECK(maxLoad <= 1.3f + 1e-3f);
}
