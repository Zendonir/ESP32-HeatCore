// Permanent manual control of heating circuits (valves): open, closed, percent.
#include "SimRig.h"
#include "hc/api/ApiRouter.h"

using namespace hc;
using sim::Rig;

namespace {
ApiResponse call(ApiRouter& api, const char* method, const char* path, const char* body = "") {
  ApiRequest r;
  r.method = method;
  r.path = path;
  r.body = body;
  return api.handle(r);
}
void feed(Rig& r, float t, uint32_t sec) {
  for (uint32_t s = 0; s < sec; ++s) {
    if (s % 30 == 0) r.sys.sensors.update("ow1:01", t, r.clock.ms);
    r.run(1000, false);
  }
}
}  // namespace

TEST(circuit_manual_open_closed_survive_restart) {
  Rig r;
  r.basicRoom();  // room "wohnen" on circuit 1 (minOn/minOff 60 s)
  ApiRouter api(r.sys);
  feed(r, 23.f, 120);  // warm: the controller keeps the valve closed
  CHECK(!r.output(1));
  // permanently open although the room is warm
  CHECK_EQ(call(api, "PUT", "/api/circuits/1/manual", R"({"mode":"open"})").status, 200);
  feed(r, 23.f, 120);
  CHECK(r.output(1));
  // stored with the heating configuration: survives a restart
  JsonDocument cfg;
  r.sys.sectionToJson(ConfigSection::Heating, cfg.to<JsonObject>(), false);
  CHECK_STR(cfg["circuits"][0]["manual"] | "", "open");
  Rig r2;
  r2.basicRoom();
  r2.load(ConfigSection::Heating, [&] { std::string s; serializeJson(cfg, s); return s; }().c_str());
  CHECK(r2.sys.heating.circuits[0].manual == CircuitManual::Open);
  // a page that does not know the field keeps it
  r.load(ConfigSection::Heating, R"({"rooms":[{"id":"wohnen","name":"W","comfort":21,"eco":18,"frost":7,"mode":"comfort"}],
                                     "circuits":[{"n":1,"room":"wohnen","minOnSec":60,"minOffSec":60}]})");
  CHECK(r.sys.heating.circuits[0].manual == CircuitManual::Open);
  // permanently closed although the room is cold
  CHECK_EQ(call(api, "PUT", "/api/circuits/1/manual", R"({"mode":"closed"})").status, 200);
  feed(r, 17.f, 180);
  CHECK(!r.output(1));
  // back to automatic: the cold room opens the valve again
  CHECK_EQ(call(api, "DELETE", "/api/circuits/1/manual").status, 200);
  feed(r, 17.f, 120);
  CHECK(r.output(1));
}

TEST(circuit_manual_percent_and_validation) {
  Rig r;
  r.basicRoom();
  ApiRouter api(r.sys);
  // 30 % of 20 min = 6 min open per cycle
  CHECK_EQ(call(api, "PUT", "/api/circuits/1/manual", R"({"mode":"percent","pct":30,"cycleMin":20})").status, 200);
  uint32_t openSec = 0;
  for (uint32_t s = 0; s < 3 * 20 * 60; ++s) {
    if (s % 30 == 0) r.sys.sensors.update("ow1:01", 23.f, r.clock.ms);
    r.run(1000, false);
    if (r.output(1)) ++openSec;
  }
  CHECK(openSec > 3 * 6 * 60 - 60 && openSec < 3 * 6 * 60 + 60);
  // a pulse shorter than the minimum on time is refused (60 s of 5 min = 20 %)
  ApiResponse a = call(api, "PUT", "/api/circuits/1/manual", R"({"mode":"percent","pct":10,"cycleMin":5})");
  CHECK_EQ(a.status, 400);
  CHECK(a.body.find("20..80") != std::string::npos);
  CHECK_EQ(call(api, "PUT", "/api/circuits/1/manual", R"({"mode":"percent","pct":0,"cycleMin":5})").status, 200);
  CHECK_EQ(call(api, "PUT", "/api/circuits/1/manual", R"({"mode":"percent","pct":101})").status, 400);
  CHECK_EQ(call(api, "PUT", "/api/circuits/1/manual", R"({"mode":"half"})").status, 400);
  // only heating circuit outputs
  r.load(ConfigSection::Io, R"({"outputs":[{"n":9,"name":"Pumpe","function":"general"}]})");
  CHECK_EQ(call(api, "PUT", "/api/circuits/9/manual", R"({"mode":"open"})").status, 400);
}

TEST(circuit_manual_safety_still_wins) {
  Rig r;
  r.basicRoom();
  ApiRouter api(r.sys);
  CHECK_EQ(call(api, "PUT", "/api/circuits/1/manual", R"({"mode":"open"})").status, 200);
  feed(r, 22.f, 120);
  CHECK(r.output(1));
  // room over-temperature: the valve closes despite manual "open"
  feed(r, r.sys.safety.cfg.maxRoomTemp + 1.f, 120);
  CHECK(!r.output(1));
}

TEST(circuit_manual_counts_as_heat_demand) {
  Rig r;
  r.basicRoom();
  r.load(ConfigSection::Mqtt, R"({"enabled":true,"host":"10.0.0.2"})");
  r.sys.mqttPublishRaw = [](const std::string&, const std::string&, bool) {};
  ApiRouter api(r.sys);
  CHECK_EQ(call(api, "PUT", "/api/heatdemand",
                R"({"enabled":true,"topic":"x/y","payloadOn":"on","payloadOff":"off","runOnSec":60,"minOnSec":60,"minOffSec":60})")
               .status,
           200);
  feed(r, 23.f, 90);
  CHECK(!r.sys.heatDemand.active);
  CHECK_EQ(call(api, "PUT", "/api/circuits/1/manual", R"({"mode":"open"})").status, 200);
  feed(r, 23.f, 10);
  CHECK(r.sys.heatDemand.active);
  CHECK(!r.sys.heatDemand.requesting.empty() && r.sys.heatDemand.requesting.back() == "hk1");
}

TEST(room_without_sensor_says_so) {
  Rig r;
  r.load(ConfigSection::Heating, R"({"rooms":[{"id":"flur","name":"Flur","comfort":21,"eco":18,"frost":7,"mode":"comfort"}],
                                     "circuits":[{"n":1,"room":"flur","minOnSec":60,"minOffSec":60}]})");
  // a sensor exists, but is not assigned to the room
  r.load(ConfigSection::Sensors, R"({"sensors":[{"id":"ow1:01","name":"Flur_Temp","timeoutSec":300}]})");
  feed(r, 20.f, 150);  // after the 2 min start-up grace
  const Room* f = r.sys.heating.room("flur");
  CHECK(f && !f->hasTemp);
  CHECK_STR(f->reason, "kein Raumfuehler zugeordnet");
  // assigned to the room: it controls
  r.load(ConfigSection::Sensors, R"({"sensors":[{"id":"ow1:01","name":"Flur_Temp","room":"flur","timeoutSec":300}]})");
  feed(r, 20.f, 60);
  CHECK(f->hasTemp);
  CHECK(f->reason.find("Raumfuehler") == std::string::npos);
}

TEST(circuit_manual_review_cases) {
  // a circuit without a room is switched by hand too
  Rig r;
  r.basicRoom();  // circuit 1 -> wohnen; circuit 2 has no room
  ApiRouter api(r.sys);
  CHECK_EQ(call(api, "PUT", "/api/circuits/2/manual", R"({"mode":"open"})").status, 200);
  feed(r, 22.f, 30);
  CHECK(r.output(2));
  CHECK_EQ(call(api, "PUT", "/api/circuits/2/manual", R"({"mode":"auto"})").status, 200);
  feed(r, 22.f, 150);
  CHECK(!r.output(2));

  // heat demand: a cold room with all valves closed by hand is no consumer,
  // a manually opened valve in an over-temperature room neither
  r.load(ConfigSection::Mqtt, R"({"enabled":true,"host":"10.0.0.2"})");
  r.sys.mqttPublishRaw = [](const std::string&, const std::string&, bool) {};
  CHECK_EQ(call(api, "PUT", "/api/heatdemand",
                R"({"enabled":true,"topic":"x/y","payloadOn":"on","payloadOff":"off","runOnSec":60,"minOnSec":60,"minOffSec":60})")
               .status,
           200);
  feed(r, 17.f, 120);
  CHECK(r.sys.heatDemand.active);  // cold room, automatic valve
  CHECK_EQ(call(api, "PUT", "/api/circuits/1/manual", R"({"mode":"closed"})").status, 200);
  feed(r, 17.f, 200);
  CHECK(!r.sys.heatDemand.active);
  CHECK_EQ(call(api, "PUT", "/api/circuits/1/manual", R"({"mode":"open"})").status, 200);
  feed(r, 22.f, 130);
  CHECK(r.sys.heatDemand.active);
  feed(r, r.sys.safety.cfg.maxRoomTemp + 1.f, 200);
  CHECK(!r.output(1));
  CHECK(!r.sys.heatDemand.active);
}
