// Heat demand to the heat source (e.g. RC35 heating circuit 2 via EMS-ESP).
#include <vector>

#include "SimRig.h"
#include "hc/api/ApiRouter.h"
#include "hc/mqtt/MqttBridge.h"

using namespace hc;
using sim::Rig;

namespace {

struct Sent {
  std::string topic, payload;
};

ApiResponse call(ApiRouter& api, const char* method, const char* path, const char* body = "") {
  ApiRequest r;
  r.method = method;
  r.path = path;
  r.body = body;
  return api.handle(r);
}

// one room "wohnen" (circuit 1, sensor ow1:01) plus MQTT capture
void setup(Rig& r, std::vector<Sent>& sent) {
  r.basicRoom();
  r.load(ConfigSection::Mqtt, R"({"enabled":true,"host":"10.0.0.2"})");
  r.sys.mqttPublishRaw = [&sent](const std::string& t, const std::string& p, bool) { sent.push_back({t, p}); };
  r.load(ConfigSection::Io, R"({"outputs":[{"n":9,"name":"Pumpe FBH","function":"general"}]})");
}

const char* kEms = R"({"enabled":true,"runOnSec":600,"minOnSec":300,"minOffSec":120,"repeatSec":300,"output":9,
  "topic":"ems-esp/thermostat","payloadOn":"{\"cmd\":\"mode\",\"data\":\"day\",\"hc\":2}",
  "payloadOff":"{\"cmd\":\"mode\",\"data\":\"night\",\"hc\":2}",
  "payloadFallback":"{\"cmd\":\"mode\",\"data\":\"auto\",\"hc\":2}"})";

void feed(Rig& r, float t, uint32_t sec) {
  for (uint32_t s = 0; s < sec; ++s) {
    if (s % 30 == 0) r.sys.sensors.update("ow1:01", t, r.clock.ms);
    r.run(1000, false);
  }
}

}  // namespace

TEST(heat_demand_switches_circuit_with_run_on) {
  Rig r;
  std::vector<Sent> sent;
  setup(r, sent);
  ApiRouter api(r.sys);
  CHECK_EQ(call(api, "PUT", "/api/heatdemand", kEms).status, 200);
  // warm room: after the start delay the circuit is told "off" once
  feed(r, 22.f, 90);
  CHECK(!r.sys.heatDemand.active);
  CHECK_EQ(sent.size(), 1u);
  CHECK(sent[0].payload.find("night") != std::string::npos);
  CHECK_STR(sent[0].topic, "ems-esp/thermostat");
  CHECK(!r.output(9));
  // the room cools down: heat demand on, pump output on
  feed(r, 19.f, 60);
  CHECK(r.sys.heatDemand.active);
  CHECK(sent.back().payload.find("day") != std::string::npos);
  CHECK(r.output(9));
  CHECK_EQ(r.sys.heatDemand.requesting.size(), 1u);
  // warm again: run-on of 10 minutes keeps the circuit on
  feed(r, 22.f, 5 * 60);
  CHECK(r.sys.heatDemand.active);
  feed(r, 22.f, 6 * 60);
  CHECK(!r.sys.heatDemand.active);
  CHECK(sent.back().payload.find("night") != std::string::npos);
  CHECK(!r.output(9));
  // the state is repeated every 5 minutes (EMS-ESP / RC35 may have been changed in between)
  size_t n = sent.size();
  feed(r, 22.f, 5 * 60 + 5);
  CHECK_EQ(sent.size(), n + 1);
  // status for the web UI and Home Assistant
  JsonDocument d;
  deserializeJson(d, call(api, "GET", "/api/heatdemand").body);
  CHECK(!(d["status"]["active"] | true));
  CHECK_STR(d["config"]["topic"] | "", "ems-esp/thermostat");
  MqttBridge br(r.sys);
  std::vector<MqttMsg> disc;
  br.discovery(disc);
  bool ent = false;
  for (auto& m : disc) ent |= m.topic.find("/heatdemand/config") != std::string::npos && !m.payload.empty();
  CHECK(ent);
}

TEST(heat_demand_min_times_test_send_and_fallback) {
  Rig r;
  std::vector<Sent> sent;
  setup(r, sent);
  ApiRouter api(r.sys);
  CHECK_EQ(call(api, "PUT", "/api/heatdemand", kEms).status, 200);
  feed(r, 19.f, 90);  // demand from the start
  CHECK(r.sys.heatDemand.active);
  // short demand ends: run-on 10 min dominates; then min-off keeps it off for 2 min
  feed(r, 22.f, 11 * 60);
  CHECK(!r.sys.heatDemand.active);
  feed(r, 19.f, 40);
  CHECK(!r.sys.heatDemand.active);  // min off 120 s
  feed(r, 19.f, 90);
  CHECK(r.sys.heatDemand.active);
  // test sending from the web UI
  size_t n = sent.size();
  CHECK_EQ(call(api, "POST", "/api/heatdemand/test", R"({"send":"fallback"})").status, 200);
  CHECK(sent.size() == n + 1 && sent.back().payload.find("auto") != std::string::npos);
  CHECK_EQ(call(api, "POST", "/api/heatdemand/test", R"({"send":"x"})").status, 400);
  // switching the function off gives the circuit back (auto) and releases the pump
  CHECK_EQ(call(api, "PUT", "/api/heatdemand", R"({"enabled":false})").status, 200);
  CHECK(sent.back().payload.find("auto") != std::string::npos);
  r.run(2000, false);
  CHECK(!r.output(9));
  // configuration survives a save/load
  CHECK_EQ(call(api, "PUT", "/api/heatdemand", kEms).status, 200);
  JsonDocument d;
  r.sys.sectionToJson(ConfigSection::Heating, d.to<JsonObject>(), false);
  Rig r2;
  r2.load(ConfigSection::Heating, d.as<std::string>().c_str());
  CHECK(r2.sys.heatDemand.cfg.enabled);
  CHECK_EQ(r2.sys.heatDemand.cfg.output, 9);
  CHECK(r2.sys.heatDemand.cfg.payloadOff.find("night") != std::string::npos);
}

TEST(heat_demand_validation_and_pwm_rooms) {
  Rig r;
  std::vector<Sent> sent;
  setup(r, sent);
  ApiRouter api(r.sys);
  // heating circuit outputs are never used as heat demand output
  CHECK_EQ(call(api, "PUT", "/api/heatdemand", R"({"enabled":true,"output":1})").status, 400);
  CHECK_EQ(call(api, "PUT", "/api/heatdemand", R"({"enabled":true,"rooms":["gibtsnicht"],"output":9})").status, 400);
  CHECK_EQ(call(api, "PUT", "/api/heatdemand", R"({"enabled":true,"topic":"a/#","payloadOn":"1","payloadOff":"0"})").status,
           400);
  CHECK_EQ(call(api, "PUT", "/api/heatdemand", R"({"enabled":true,"topic":"a/b","payloadOn":"1"})").status, 400);
  CHECK_EQ(call(api, "PUT", "/api/heatdemand", R"({"enabled":true})").status, 400);  // neither topic nor output
  CHECK_EQ(call(api, "PUT", "/api/heatdemand", R"({"enabled":true,"output":9,"repeatSec":5})").status, 400);
  // MQTT off: nothing is sent, the status says why
  r.load(ConfigSection::Mqtt, R"({"enabled":false})");
  CHECK_EQ(call(api, "PUT", "/api/heatdemand", kEms).status, 200);
  feed(r, 19.f, 90);
  CHECK(sent.empty());
  CHECK(r.sys.heatDemand.lastSent.find("MQTT") != std::string::npos);
  CHECK(r.output(9));  // the output still follows
  // PWM room: the demand counts for the whole cycle, not only while the pulse is on
  r.load(ConfigSection::Heating, R"({"rooms":[{"id":"wohnen","name":"W","comfort":21,"eco":18,"frost":7,"mode":"comfort",
      "control":"pwm","pwmCycleMin":20,"pwmKp":40,"pwmTnMin":0}],"circuits":[{"n":1,"room":"wohnen","minOnSec":60,"minOffSec":60}]})");
  CHECK_EQ(call(api, "PUT", "/api/heatdemand", kEms).status, 200);
  feed(r, 20.6f, 20 * 60);  // 0.4 K below: 16 % duty, valve mostly closed
  CHECK(r.sys.heatDemand.active);
}
