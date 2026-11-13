// Heat supply check: valves stay closed while the heat source delivers no heat.
#include "SimRig.h"
#include "hc/api/ApiRouter.h"
#include "hc/mqtt/MqttBridge.h"

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

// cold room "wohnen" (circuit 1, room sensor ow1:01) and a boiler flow sensor
void setup(Rig& r) {
  r.load(ConfigSection::Heating,
         R"({"rooms":[{"id":"wohnen","name":"Wohnen","comfort":21,"eco":18,"frost":7,"mode":"comfort","hysteresis":0.4}],
             "circuits":[{"n":1,"room":"wohnen","minOnSec":60,"minOffSec":60}]})");
  r.load(ConfigSection::Sensors, R"({"sensors":[{"id":"ow1:01","name":"Fuehler","room":"wohnen","timeoutSec":300},
             {"id":"ow1:02","name":"Kesselvorlauf","timeoutSec":300}]})");
}

void feed(Rig& r, float flow, uint32_t sec) {
  for (uint32_t s = 0; s < sec; ++s) {
    if (s % 30 == 0) {
      r.sys.sensors.update("ow1:01", 18.f, r.clock.ms);
      r.sys.sensors.update("ow1:02", flow, r.clock.ms);
    }
    r.run(1000, false);
  }
}

std::string thermo(int targetFlow) {
  return std::string(R"j({"hc2":{"mode":"night","reducemode":"nofrost","targetflowtemp":)j") +
         std::to_string(targetFlow) + "}}";
}

}  // namespace

TEST(heat_supply_sensor_closes_and_reopens_valves) {
  Rig r;
  setup(r);
  ApiRouter api(r.sys);
  CHECK_EQ(call(api, "PUT", "/api/heatdemand",
                R"({"enabled":false,"supply":"sensor","supplySensor":"ow1:02","supplyMinTemp":25,"supplyDelaySec":300})")
               .status,
           200);
  // hot boiler: the cold room heats
  feed(r, 45.f, 180);
  CHECK(r.sys.heating.supplyOk);
  CHECK(r.output(1));
  // boiler cold: valves stay open for the delay, then close
  feed(r, 20.f, 240);
  CHECK(r.output(1));
  feed(r, 20.f, 120);
  CHECK(!r.sys.heating.supplyOk);
  CHECK(!r.output(1));
  const Room* w = r.sys.heating.room("wohnen");
  CHECK(w->demand);  // the room still needs heat
  CHECK(w->reason.find("kein Waermeangebot") != std::string::npos);
  // heat returns: valves open again at once (after the minimum off time)
  feed(r, 40.f, 90);
  CHECK(r.sys.heating.supplyOk);
  CHECK(r.output(1));
  // supply sensor fails: valves work normally (no lock-out on a sensor fault)
  feed(r, 20.f, 400);
  CHECK(!r.output(1));
  for (uint32_t s = 0; s < 420; ++s) {
    if (s % 30 == 0) r.sys.sensors.update("ow1:01", 18.f, r.clock.ms);
    r.run(1000, false);
  }
  CHECK(r.sys.heating.supplyOk);
  CHECK(r.output(1));
  // status for the web UI
  JsonDocument d;
  deserializeJson(d, call(api, "GET", "/api/heatdemand").body);
  CHECK_STR(std::string(d["status"]["supply"]["mode"] | ""), "sensor");
  CHECK(d["status"]["supply"]["ok"] == true);
  CHECK(std::string(d["status"]["supply"]["text"] | "").find("normal") != std::string::npos);
}

TEST(heat_supply_emsesp_target_flow_and_pump) {
  Rig r;
  setup(r);
  ApiRouter api(r.sys);
  CHECK_EQ(call(api, "PUT", "/api/emsesp", R"({"enabled":true,"base":"ems-esp","hc":2})").status, 200);
  CHECK_EQ(call(api, "PUT", "/api/heatdemand", R"({"supply":"emsesp","supplyMinTemp":25,"supplyDelaySec":60})").status,
           200);
  auto ems = [&](int target, const char* pump, uint32_t sec) {
    for (uint32_t s = 0; s < sec; ++s) {
      if (s % 10 == 0) {
        r.sys.emsIngest("ems-esp/thermostat_data", thermo(target));
        r.sys.emsIngest("ems-esp/mixer_data", std::string(R"j({"hc2":{"flowtemphc":30,"pumpstatus":")j") + pump + "\"}}");
      }
      feed(r, 40.f, 1);
    }
  };
  ems(35, "on", 150);
  CHECK(r.sys.heating.supplyOk);
  CHECK(r.output(1));
  // RC35 lowers the calculated flow temperature (night setback): valves close
  ems(21, "on", 90);
  CHECK(!r.sys.heating.supplyOk);
  CHECK(!r.output(1));
  CHECK(r.sys.heatDemand.supplyText.find("21") != std::string::npos);
  ems(35, "on", 70);
  CHECK(r.output(1));
  // pump of the heating circuit off: no heat either
  ems(35, "off", 90);
  CHECK(!r.output(1));
  // Home Assistant: binary sensor and room power 0 while there is no heat
  MqttBridge br(r.sys);
  std::vector<MqttMsg> msgs;
  br.discovery(msgs);
  bool disc = false;
  for (auto& m : msgs) disc |= m.topic.find("binary_sensor/heatcore_") != std::string::npos &&
                                m.topic.find("/heatsupply/config") != std::string::npos && !m.payload.empty();
  CHECK(disc);
  std::vector<MqttMsg> st;
  br.state(st);
  std::string supply, power;
  for (auto& m : st) {
    JsonDocument d;
    deserializeJson(d, m.payload);
    if (m.topic.find("/system/state") != std::string::npos) supply = d["supply"] | "";
    if (m.topic.find("/room/wohnen/state") != std::string::npos) power = std::to_string(d["power"] | -1);
  }
  CHECK_STR(supply, "OFF");
  CHECK_STR(power, "0");
  // validation
  CHECK_EQ(call(api, "PUT", "/api/heatdemand", R"({"supply":"sensor","supplySensor":""})").status, 400);
  CHECK_EQ(call(api, "PUT", "/api/heatdemand", R"({"supply":"oil"})").status, 400);
  CHECK_EQ(call(api, "PUT", "/api/heatdemand", R"({"supply":"emsesp","supplyMinTemp":5})").status, 400);
  // switched off: valves normal again
  CHECK_EQ(call(api, "PUT", "/api/heatdemand", R"({"supply":"off"})").status, 200);
  ems(35, "off", 70);
  CHECK(r.sys.heating.supplyOk);
  CHECK(r.output(1));
}
