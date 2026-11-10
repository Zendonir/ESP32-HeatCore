// EMS-ESP connection: reading boiler / thermostat / mixer data and switching
// a heating circuit of an RC35 directly for the heat demand.
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

// payloads as published by EMS-ESP 3.8 (German, nested format), shortened
const char* kBoiler =
    R"j({"reset":"","heatingoff":"aus","heatingactive":"an","tapwateractive":"aus","selflowtemp":46,"outdoortemp":5.7,)j"
    R"j("curflowtemp":47.7,"rettemp":43,"syspress":1.5,"burngas":"an","curburnpow":20,"servicecode":"-H",)j"
    R"j("lastcode":"9H(267) 14.07.2025 08:41 (0 min)","dhw":{"settemp":60,"curtemp":59.6,"charging":"aus"}})j";
const char* kMixer = R"j({"hc2":{"flowtemphc":27.4,"valvestatus":6,"flowsettemp":28,"pumpstatus":"an","activated":"an"}})j";

std::string thermostat(const char* mode2, const char* reduce2, const char* modetype2 = "Tag") {
  return std::string(R"j({"lastcode":"32(807) 01.08.2025 13:38 (243 min)","dampedoutdoortemp":5,"building":"Mittel",)j") +
         R"j("hc1":{"seltemp":19.5,"mode":"auto","modetype":"Tag","designtemp":75,"heatingtype":"Heizkörper","reducemode":"Außen"},)j" +
         R"j("hc2":{"seltemp":19.5,"currtemp":19.8,"mode":")j" + mode2 + R"j(","modetype":")j" + modetype2 +
         R"j(","designtemp":40,"targetflowtemp":28,"summermode":"Winter","heatingtype":"Fussboden","reducemode":")j" +
         reduce2 + R"j("},"dhw":{"mode":"auto"}})j";
}

// one room "wohnen" (circuit 1, sensor ow1:01), MQTT on and captured, EMS-ESP for heating circuit 2
void setup(Rig& r, std::vector<Sent>& sent, ApiRouter& api) {
  r.basicRoom();
  r.load(ConfigSection::Mqtt, R"j({"enabled":true,"host":"10.0.0.2"})j");
  r.sys.mqttPublishRaw = [&sent](const std::string& t, const std::string& p, bool) { sent.push_back({t, p}); };
  CHECK_EQ(call(api, "PUT", "/api/emsesp", R"j({"enabled":true,"base":"ems-esp","hc":2})j").status, 200);
}

const char* kDemand = R"j({"enabled":true,"target":"emsesp","runOnSec":600,"minOnSec":300,"minOffSec":120,"repeatSec":300})j";

// The RC35 as EMS-ESP shows it: follows the commands HeatCore sends (unless frozen).
struct Rc35 {
  std::string mode = "auto", reduce = "Außen";
  bool follow = true;
  size_t seen = 0;
  void apply(const std::vector<Sent>& sent) {
    for (; seen < sent.size(); ++seen) {
      if (!follow) continue;
      const Sent& m = sent[seen];
      if (m.topic == "ems-esp/thermostat/hc2/mode")
        mode = m.payload == "day" ? "Tag" : m.payload == "night" ? "Nacht" : "auto";
      if (m.topic == "ems-esp/thermostat/hc2/reducemode")
        reduce = m.payload == "nofrost" ? "Frostschutz" : m.payload == "outdoor" ? "Außen" : m.payload;
    }
  }
};

// run `sec` seconds: room temperature every 30 s, EMS-ESP data every 10 s
void feed(Rig& r, float t, uint32_t sec, Rc35* rc = nullptr, std::vector<Sent>* sent = nullptr) {
  for (uint32_t s = 0; s < sec; ++s) {
    if (s % 30 == 0) r.sys.sensors.update("ow1:01", t, r.clock.ms);
    if (rc && s % 10 == 0) {
      rc->apply(*sent);
      r.sys.emsIngest("ems-esp/thermostat_data", thermostat(rc->mode.c_str(), rc->reduce.c_str()));
    }
    r.run(1000, false);
  }
}

size_t countTopic(const std::vector<Sent>& sent, size_t from, const std::string& topic) {
  size_t n = 0;
  for (size_t i = from; i < sent.size(); ++i)
    if (sent[i].topic == topic) ++n;
  return n;
}

}  // namespace

TEST(emsesp_parses_boiler_thermostat_mixer) {
  Rig r;
  std::vector<Sent> sent;
  ApiRouter api(r.sys);
  setup(r, sent, api);
  MqttBridge br(r.sys);
  auto subs = br.subscriptions();
  CHECK(std::find(subs.begin(), subs.end(), "ems-esp/thermostat_data") != subs.end());
  CHECK(std::find(subs.begin(), subs.end(), "ems-esp/boiler_data") != subs.end());
  CHECK(std::find(subs.begin(), subs.end(), "ems-esp/mixer_data") != subs.end());

  CHECK(br.handle("ems-esp/boiler_data", kBoiler));
  CHECK(br.handle("ems-esp/thermostat_data", thermostat("auto", "Außen")));
  CHECK(br.handle("ems-esp/mixer_data", kMixer));
  CHECK(br.handle("ems-esp/status", "online"));
  const EmsEspState& st = r.sys.ems.st;
  CHECK_NEAR(st.outdoor, 5.7f, 0.01f);
  CHECK_NEAR(st.boilerFlow, 47.7f, 0.01f);
  CHECK_NEAR(st.dhwTemp, 59.6f, 0.01f);
  CHECK_NEAR(st.pressure, 1.5f, 0.01f);
  CHECK_EQ(st.burner, 1);
  CHECK(st.mode == EmsMode::Auto);
  CHECK(st.reduce == EmsReduce::Outdoor);  // "Außen" (German, UTF-8)
  CHECK_STR(st.heatingType, "Fussboden");  // circuit 2, not circuit 1
  CHECK_NEAR(st.targetFlow, 28.f, 0.01f);
  CHECK_NEAR(st.hcFlow, 27.4f, 0.01f);
  CHECK_EQ(st.pump, 1);
  CHECK_EQ(st.online, 1);
  CHECK(r.sys.ems.thermostatFresh(r.clock.ms));

  // the values are offered as new sensors and can be taken over
  bool found = false;
  for (auto& d : r.sys.sensors.discovered())
    if (d.id == "ems:outdoor") found = true;
  CHECK(found);
  JsonDocument d;
  deserializeJson(d, call(api, "POST", "/api/emsesp/sensors").body);
  CHECK_EQ(d["added"].size(), 4u);
  const Sensor* out = r.sys.sensors.find("ems:outdoor");
  CHECK(out && out->role == SensorRole::Outdoor);
  CHECK(r.sys.sensors.find("ems:hcflow") && r.sys.sensors.find("ems:hcflow")->role == SensorRole::Other);
  br.handle("ems-esp/boiler_data", kBoiler);
  CHECK(out->available());

  // status for the web UI
  d.clear();
  deserializeJson(d, call(api, "GET", "/api/emsesp").body);
  CHECK_STR(d["status"]["thermostat"]["mode"] | "", "auto");
  CHECK(d["status"]["mixer"]["pump"] | false);
  CHECK(d["config"]["savedReduce"].isNull());

  // English texts and enums as index (other EMS-ESP settings)
  r.sys.emsIngest("ems-esp/thermostat_data", R"j({"hc2":{"mode":"night","reducemode":"nofrost"}})j");
  CHECK(st.mode == EmsMode::Night);
  CHECK(st.reduce == EmsReduce::NoFrost);
  r.sys.emsIngest("ems-esp/thermostat_data", R"j({"hc2":{"mode":1,"reducemode":3}})j");
  CHECK(st.mode == EmsMode::Day);
  CHECK(st.reduce == EmsReduce::Outdoor);
  // other topics of the same base are not EMS data, foreign topics untouched
  CHECK(!r.sys.emsIngest("ems-esp/heartbeat", "{}"));
  CHECK(!r.sys.emsIngest("zigbee2mqtt/x", "{}"));
  // garbage does not crash and is reported
  CHECK(r.sys.emsIngest("ems-esp/boiler_data", "23.5"));
  CHECK(!r.sys.ems.st.error.empty());
}

TEST(emsesp_heat_demand_takes_over_switches_and_gives_back) {
  Rig r;
  std::vector<Sent> sent;
  ApiRouter api(r.sys);
  setup(r, sent, api);
  CHECK_EQ(call(api, "PUT", "/api/heatdemand", kDemand).status, 200);
  // no thermostat data yet: nothing is sent, the circuit is not taken over blindly
  feed(r, 22.f, 90);
  CHECK(sent.empty());
  JsonDocument d;
  deserializeJson(d, call(api, "GET", "/api/heatdemand").body);
  CHECK(std::string(d["status"]["lastSent"] | "").find("warte auf Daten") != std::string::npos);

  // data arrive: take over (remember auto / outdoor), switch-off reduction, night
  Rc35 rc;
  feed(r, 22.f, 30, &rc, &sent);
  CHECK_STR(r.sys.ems.cfg.savedMode, "auto");
  CHECK_STR(r.sys.ems.cfg.savedReduce, "outdoor");
  CHECK_EQ(countTopic(sent, 0, "ems-esp/thermostat/hc2/reducemode"), 1u);
  CHECK_STR(sent.back().topic, "ems-esp/thermostat/hc2/mode");
  CHECK_STR(sent.back().payload, "night");

  // confirmed by the thermostat: no repetition (every command is written into the RC35)
  size_t n = sent.size();
  feed(r, 22.f, 15 * 60, &rc, &sent);
  CHECK_EQ(sent.size(), n);
  d.clear();
  deserializeJson(d, call(api, "GET", "/api/heatdemand").body);
  CHECK(d["status"]["ems"]["confirmed"] | false);
  CHECK(d["status"]["ems"]["takenOver"] | false);

  // the room cools down: day
  feed(r, 19.f, 60, &rc, &sent);
  CHECK(r.sys.heatDemand.active);
  CHECK_STR(sent.back().payload, "day");
  CHECK_EQ(countTopic(sent, 0, "ems-esp/thermostat/hc2/reducemode"), 1u);  // already switch-off reduction

  // somebody switches the circuit at the RC35 (and the RC35 ignores HeatCore for a
  // while): corrected at once, then at most once per minute
  n = sent.size();
  rc.mode = "Nacht";
  rc.follow = false;
  feed(r, 19.f, 20, &rc, &sent);
  CHECK_EQ(sent.size(), n + 1);
  CHECK_STR(sent.back().payload, "day");
  feed(r, 19.f, 40, &rc, &sent);
  CHECK_EQ(sent.size(), n + 1);
  feed(r, 19.f, 30, &rc, &sent);
  CHECK_EQ(sent.size(), n + 2);
  // the RC35 takes the next correction: quiet again
  rc.follow = true;
  feed(r, 19.f, 70, &rc, &sent);
  CHECK_EQ(sent.size(), n + 3);
  CHECK_STR(rc.mode, "Tag");
  feed(r, 19.f, 5 * 60, &rc, &sent);
  CHECK_EQ(sent.size(), n + 3);

  // the configuration (with the remembered modes) survives a restart
  JsonDocument cfg;
  r.sys.sectionToJson(ConfigSection::Heating, cfg.to<JsonObject>(), false);
  CHECK_STR(cfg["emsesp"]["savedReduce"] | "", "outdoor");
  CHECK_STR(cfg["heatDemand"]["target"] | "", "emsesp");

  // the EMS-ESP connection cannot be switched off while it is used
  CHECK_EQ(call(api, "PUT", "/api/emsesp", R"j({"enabled":false})j").status, 400);

  // heat demand off: the circuit is given back exactly as it was
  n = sent.size();
  CHECK_EQ(call(api, "PUT", "/api/heatdemand", R"j({"enabled":false,"target":"emsesp"})j").status, 200);
  CHECK_EQ(sent.size(), n + 2);
  CHECK_STR(sent[n].topic, "ems-esp/thermostat/hc2/mode");
  CHECK_STR(sent[n].payload, "auto");
  CHECK_STR(sent[n + 1].topic, "ems-esp/thermostat/hc2/reducemode");
  CHECK_STR(sent[n + 1].payload, "outdoor");
  CHECK(r.sys.ems.cfg.savedReduce.empty());
  feed(r, 19.f, 10 * 60, &rc, &sent);
  CHECK_EQ(sent.size(), n + 2);  // nothing more
}

TEST(emsesp_restart_keeps_original_modes) {
  // HeatCore restarts while it controls the circuit (RC35 still in night / switch-off):
  // the stored original modes must not be replaced by HeatCore's own ones
  Rig r;
  std::vector<Sent> sent;
  ApiRouter api(r.sys);
  setup(r, sent, api);
  r.load(ConfigSection::Heating,
         R"j({"rooms":[{"id":"wohnen","name":"W","comfort":21,"eco":18,"frost":7,"mode":"comfort"}],
             "circuits":[{"n":1,"room":"wohnen","minOnSec":60,"minOffSec":60}],
             "emsesp":{"enabled":true,"base":"ems-esp","hc":2,"savedMode":"auto","savedReduce":"room"},
             "heatDemand":{"enabled":true,"target":"emsesp","runOnSec":600,"minOnSec":300,"minOffSec":120}})j");
  Rc35 rc;
  rc.mode = "Nacht";
  rc.reduce = "Frostschutz";
  feed(r, 22.f, 90, &rc, &sent);
  CHECK_STR(r.sys.ems.cfg.savedReduce, "room");
  CHECK_STR(r.sys.ems.cfg.savedMode, "auto");
  CHECK_EQ(countTopic(sent, 0, "ems-esp/thermostat/hc2/reducemode"), 0u);  // already switch-off
  // controlled restart: given back to the original reduction mode
  size_t n = sent.size();
  r.sys.heatDemandRelease();
  CHECK_EQ(sent.size(), n + 2);
  CHECK_STR(sent[n + 1].payload, "room");
}

TEST(emsesp_validation) {
  Rig r;
  std::vector<Sent> sent;
  ApiRouter api(r.sys);
  r.basicRoom();
  // heat demand to EMS-ESP needs the connection
  CHECK_EQ(call(api, "PUT", "/api/heatdemand", kDemand).status, 400);
  CHECK_EQ(call(api, "PUT", "/api/emsesp", R"j({"enabled":true,"base":"ems/#","hc":2})j").status, 400);
  CHECK_EQ(call(api, "PUT", "/api/emsesp", R"j({"enabled":true,"base":"ems-esp","hc":7})j").status, 400);
  CHECK_EQ(call(api, "PUT", "/api/emsesp", R"j({"enabled":true,"base":"ems-esp","hc":2})j").status, 200);
  CHECK_EQ(call(api, "PUT", "/api/heatdemand", kDemand).status, 200);
  // MQTT off: reported, nothing sent
  r.sys.mqttPublishRaw = [&sent](const std::string& t, const std::string& p, bool) { sent.push_back({t, p}); };
  r.sys.emsIngest("ems-esp/thermostat_data", thermostat("auto", "Außen"));
  JsonDocument d;
  deserializeJson(d, call(api, "POST", "/api/heatdemand/test", R"j({"send":"on"})j").body);
  CHECK(std::string(d["error"] | "").find("MQTT") != std::string::npos);
  CHECK(sent.empty());
  // a thermostat without day/night (unknown mode) is not taken over
  r.load(ConfigSection::Mqtt, R"j({"enabled":true,"host":"10.0.0.2"})j");
  r.sys.emsIngest("ems-esp/thermostat_data", R"j({"hc2":{"mode":"manual","reducemode":"Außen"}})j");
  d.clear();
  deserializeJson(d, call(api, "POST", "/api/heatdemand/test", R"j({"send":"on"})j").body);
  CHECK(std::string(d["error"] | "").find("nicht erkannt") != std::string::npos);
  CHECK(sent.empty());
  // known again: the test button takes over and switches
  r.sys.emsIngest("ems-esp/thermostat_data", thermostat("auto", "Außen"));
  CHECK_EQ(call(api, "POST", "/api/heatdemand/test", R"j({"send":"on"})j").status, 200);
  CHECK_EQ(sent.size(), 2u);
  CHECK_STR(sent[1].payload, "day");
}

TEST(emsesp_output_change_keeps_circuit_and_retake_waits_for_report) {
  Rig r;
  std::vector<Sent> sent;
  ApiRouter api(r.sys);
  setup(r, sent, api);
  r.load(ConfigSection::Io, R"j({"outputs":[{"n":9,"name":"Pumpe","function":"general"},{"n":10,"name":"Pumpe 2","function":"general"}]})j");
  CHECK_EQ(call(api, "PUT", "/api/heatdemand", R"j({"enabled":true,"target":"emsesp","output":9,"runOnSec":600,"minOnSec":300,"minOffSec":120})j").status, 200);
  Rc35 rc;
  feed(r, 19.f, 120, &rc, &sent);
  CHECK(r.sys.heatDemand.active);
  CHECK(r.output(9));
  CHECK_STR(r.sys.ems.cfg.savedReduce, "outdoor");
  // only the pump output changes: the heating circuit stays taken over, nothing is sent
  size_t n = sent.size();
  CHECK_EQ(call(api, "PUT", "/api/heatdemand", R"j({"enabled":true,"target":"emsesp","output":10,"runOnSec":600,"minOnSec":300,"minOffSec":120})j").status, 200);
  CHECK_EQ(sent.size(), n);
  CHECK_STR(r.sys.ems.cfg.savedReduce, "outdoor");
  CHECK_STR(r.sys.ems.cfg.savedMode, "auto");
  r.run(2000, false);
  CHECK(!r.output(9));
  CHECK(r.output(10));
  // given back by the test button and taken over again at once: the cached
  // report (HeatCore's own day / switch-off) must not become the "original"
  CHECK_EQ(call(api, "POST", "/api/heatdemand/test", R"j({"send":"fallback"})j").status, 200);
  CHECK(r.sys.ems.cfg.savedReduce.empty());
  r.run(3000, false);  // no report in between
  CHECK(r.sys.ems.cfg.savedReduce.empty());
  feed(r, 19.f, 90, &rc, &sent);  // the RC35 reports auto / outdoor again; taken over after a minute
  CHECK_STR(r.sys.ems.cfg.savedReduce, "outdoor");
  CHECK_STR(r.sys.ems.cfg.savedMode, "auto");
}
