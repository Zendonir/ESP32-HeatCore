#include "SimRig.h"
#include "hc/api/ApiRouter.h"
#include "hc/mqtt/MqttBridge.h"
#include "hc/sensors/ExternalSources.h"

using namespace hc;
using sim::Rig;

TEST(external_extract_values) {
  std::string t;
  bool num = false;
  float v = 0;
  CHECK(ExternalSources::extract("21.3", "", t, num, v) && num && std::fabs(v - 21.3f) < 1e-4f);
  CHECK(ExternalSources::extract(" 21,5 °C\n", "", t, num, v) && num && std::fabs(v - 21.5f) < 1e-4f);
  CHECK(ExternalSources::extract("48 %", "", t, num, v) && num && v == 48.f);
  CHECK(ExternalSources::extract("click", "", t, num, v) && !num && t == "click");
  CHECK(ExternalSources::extract(R"({"id":200,"value":21.4,"last_updated_ts":1})", "value", t, num, v) && num &&
        std::fabs(v - 21.4f) < 1e-4f);
  // Shelly Gen3 RPC notification: component names contain ':'
  const char* rpc = R"({"src":"shelly1g3-x","method":"NotifyStatus","params":{"ts":1,"bthomesensor:201":{"id":201,"value":47}}})";
  CHECK(ExternalSources::extract(rpc, "params.bthomesensor:201.value", t, num, v) && v == 47.f);
  CHECK(!ExternalSources::extract(rpc, "params.bthomesensor:200.value", t, num, v));  // other component
  CHECK(ExternalSources::extract(R"({"a":[{"b":"22.5"}]})", "a.0.b", t, num, v) && num && v == 22.5f);
  CHECK(ExternalSources::extract(R"({"on":true})", "on", t, num, v) && t == "true");
  CHECK(!ExternalSources::extract("kein json", "value", t, num, v));
}

TEST(external_shelly_blu_via_mqtt_and_button_plus) {
  Rig r;
  r.basicRoom();
  r.load(ConfigSection::Sensors, R"({"sensors":[{"id":"ow1:01","name":"F","room":"wohnen","timeoutSec":300}],
    "external":[
     {"name":"bad_t","topic":"shellyplus1-aa/status/bthomesensor:200","kind":"temperature","path":"value"},
     {"name":"bad_h","topic":"shellyplus1-aa/status/bthomesensor:201","kind":"humidity","path":"value","sensor":"bad_t"},
     {"name":"bad_b","topic":"shellyplus1-aa/status/bthomesensor:202","kind":"battery","path":"value","sensor":"bad_t"},
     {"name":"gen3_t","topic":"shelly1g3-x/events/rpc","kind":"temperature","path":"params.bthomesensor:200.value"},
     {"name":"flur_t","topic":"buttonplus/flur/temperature","kind":"temperature"},
     {"name":"flur_up","topic":"buttonplus/flur/button/3/click","kind":"trigger"}]})");
  MqttBridge br(r.sys);
  // all external topics are subscribed (once each)
  auto subs = br.subscriptions();
  CHECK(std::count(subs.begin(), subs.end(), std::string("shelly1g3-x/events/rpc")) == 1);
  CHECK(std::count(subs.begin(), subs.end(), std::string("buttonplus/flur/button/3/click")) == 1);
  // values arrive: sensors are offered for adoption
  CHECK(br.handle("shellyplus1-aa/status/bthomesensor:201", R"({"id":201,"value":55})"));
  CHECK(br.handle("shellyplus1-aa/status/bthomesensor:200", R"({"id":200,"value":19.8})"));
  CHECK(br.handle("buttonplus/flur/temperature", "20.6"));
  bool offered = false;
  for (auto& d : r.sys.sensors.discovered())
    if (d.id == "ext:bad_t" && d.hasHumidity && d.humidity == 55.f) offered = true;
  CHECK(offered);
  CHECK(r.sys.sensors.adopt("ext:bad_t", "Bad Shelly", "wohnen", SensorRole::Room).empty());
  CHECK(br.handle("shellyplus1-aa/status/bthomesensor:200", R"({"id":200,"value":20.1})"));
  CHECK(br.handle("shellyplus1-aa/status/bthomesensor:202", R"({"id":202,"value":12})"));
  const Sensor* s = r.sys.sensors.find("ext:bad_t");
  CHECK(s && s->available());
  CHECK(s && std::fabs(s->value - 20.1f) < 1e-3f);
  CHECK(s && s->hasHumidity && s->humidity == 55.f);
  CHECK(s && s->batteryOk == 0);  // 12 % -> low
  // Gen3 notification about another component is ignored without error
  CHECK(br.handle("shelly1g3-x/events/rpc", R"({"method":"NotifyStatus","params":{"switch:0":{"output":true}}})"));
  // implausible value is rejected and reported
  br.handle("buttonplus/flur/temperature", "999");
  JsonDocument st;
  r.sys.external.statusJson(st.to<JsonArray>(), r.clock.ms);
  bool flagged = false;
  for (JsonObject x : st.as<JsonArray>())
    if (std::string(x["name"] | "") == "flur_t" && std::string(x["error"] | "").find("unplausibel") != std::string::npos) flagged = true;
  CHECK(flagged);
  // Button+ key drives a rule through the existing MQTT trigger
  r.load(ConfigSection::Rules, R"({"rules":[{"id":1,"name":"Flur +","enabled":true,"cooldownSec":0,
     "triggers":[{"type":"mqtt","topic":"flur_up"}],"actions":[{"type":"adjust_setpoint","room":"wohnen","delta":0.5}]}]})");
  float before = r.sys.heating.room("wohnen")->comfort;
  r.run(500, false);
  float sp0 = r.sys.heating.room("wohnen")->setpoint;
  CHECK(br.handle("buttonplus/flur/button/3/click", "1"));
  r.run(11000, false);  // next control cycle applies the new setpoint
  CHECK_NEAR(r.sys.heating.room("wohnen")->setpoint, sp0 + 0.5f, 0.01f);
  (void)before;
  // configuration survives the round trip (sensors section)
  JsonDocument cfg;
  r.sys.sectionToJson(ConfigSection::Sensors, cfg.to<JsonObject>(), true);
  CHECK_EQ(cfg["external"].size(), 6u);
}

TEST(external_http_poll_and_validation) {
  Rig r;
  r.basicRoom();
  r.load(ConfigSection::Sensors, R"({"sensors":[{"id":"ow1:01","name":"F","room":"wohnen"}],"external":[
     {"name":"kueche","source":"http","topic":"http://192.168.3.40/rpc/BTHomeSensor.GetStatus?id=200","kind":"temperature","path":"value","intervalSec":30}]})");
  auto due = r.sys.external.dueHttp(r.clock.ms);
  CHECK_EQ(due.size(), 1u);
  CHECK(r.sys.external.dueHttp(r.clock.ms + 1000).empty());       // not again before the interval
  CHECK_EQ(r.sys.external.dueHttp(r.clock.ms + 31000).size(), 1u);
  r.sys.external.ingestHttp(r.sys, 0, 200, R"({"id":200,"value":22.7})", r.clock.ms);
  bool offered = false;
  for (auto& d : r.sys.sensors.discovered())
    if (d.id == "ext:kueche" && std::fabs(d.lastValue - 22.7f) < 1e-3f) offered = true;
  CHECK(offered);
  r.sys.external.ingestHttp(r.sys, 0, -1, "", r.clock.ms);
  JsonDocument st;
  r.sys.external.statusJson(st.to<JsonArray>(), r.clock.ms);
  CHECK_STR(st[0]["error"] | "", "keine Antwort");
  // validation
  auto bad = [&](const char* json, const char* expect) {
    JsonDocument d;
    deserializeJson(d, json);
    std::string e = r.sys.external.fromJson(d.as<JsonArrayConst>());
    if (e.find(expect) == std::string::npos) std::printf("    unexpected: %s\n", e.c_str());
    CHECK(e.find(expect) != std::string::npos);
  };
  bad(R"([{"name":"Gross","topic":"a/b"}])", "Name");
  bad(R"([{"name":"a","topic":"a/#"}])", "Platzhalter");
  bad(R"([{"name":"a","topic":"a/b"},{"name":"a","topic":"c"}])", "doppelt");
  bad(R"([{"name":"h","topic":"x","kind":"humidity"}])", "Temperaturquelle");
  bad(R"([{"name":"h","topic":"x","kind":"humidity","sensor":"t"},{"name":"t","topic":"y","kind":"trigger"}])", "keine Temperaturquelle");
  bad(R"([{"name":"w","source":"http","topic":"https://cloud.example/x"}])", "http://");
  bad(R"([{"name":"w","source":"http","topic":"http://1.2.3.4/x","kind":"trigger"}])", "MQTT");
  bad(R"([{"name":"w","topic":"x","kind":"pressure"}])", "Art");
  CHECK_EQ(r.sys.external.list.size(), 1u);  // failed updates leave the list unchanged
  // REST
  ApiRouter api(r.sys);
  ApiRequest q;
  q.method = "PUT";
  q.path = "/api/external";
  q.body = R"({"sources":[{"name":"t1","topic":"zigbee2mqtt/wohnen","kind":"temperature","path":"temperature"}]})";
  CHECK_EQ(api.handle(q).status, 200);
  q.method = "GET";
  q.body = "";
  ApiResponse g = api.handle(q);
  CHECK(g.body.find("zigbee2mqtt/wohnen") != std::string::npos);
}

TEST(mqtt_room_value_topics_for_displays) {
  Rig r;
  r.basicRoom();
  r.plants["wohnen"].T = 20.f;
  r.run(65000);
  MqttBridge br(r.sys);
  std::vector<MqttMsg> out;
  br.state(out);
  auto val = [&](const std::string& suffix) {
    for (auto& m : out)
      if (m.topic == r.sys.baseTopic() + "/room/wohnen/" + suffix) return m.payload;
    return std::string("<fehlt>");
  };
  char t[16], tde[16];
  float T = r.sys.heating.room("wohnen")->temperature;
  std::snprintf(t, sizeof(t), "%.1f", static_cast<double>(T));
  std::snprintf(tde, sizeof(tde), "%.1f", static_cast<double>(T));
  for (char* c = tde; *c; ++c)
    if (*c == '.') *c = ',';
  CHECK_STR(val("temp"), t);
  CHECK_STR(val("setpoint"), "21.0");
  CHECK_STR(val("heating"), "1");
  CHECK_STR(val("window"), "0");
  CHECK_STR(val("mode_text"), "Komfort");
  CHECK_STR(val("text"), std::string(tde) + "° / 21,0° heizt");
  // can be switched off
  r.load(ConfigSection::Mqtt, R"({"roomValueTopics":false})");
  out.clear();
  br.state(out);
  CHECK_STR(val("temp"), "<fehlt>");
}
