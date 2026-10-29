#include "SimRig.h"
#include "hc/api/ApiRouter.h"
#include "hc/mqtt/MqttBridge.h"

using namespace hc;
using sim::Rig;

namespace {
ApiResponse call(ApiRouter& api, const char* method, const char* path, const char* body = "",
                 std::map<std::string, std::string> q = {}) {
  ApiRequest r;
  r.method = method;
  r.path = path;
  r.body = body;
  r.query = std::move(q);
  return api.handle(r);
}
JsonDocument parse(const ApiResponse& r) {
  JsonDocument d;
  deserializeJson(d, r.body);
  return d;
}
}  // namespace

TEST(api_rf_learn_add_change_remove_without_firmware_change) {
  Rig r;
  r.basicRoom();
  ApiRouter api(r.sys);
  CHECK_EQ(call(api, "POST", "/api/rf/learn/start", R"({"timeoutSec":60})").status, 200);
  r.sendRf(0xE5A3C1);
  r.run(500, false);
  r.sendRf(0xE5A3C1);
  auto learn = parse(call(api, "GET", "/api/rf/learn"));
  CHECK(learn["active"] | false);
  CHECK_EQ(learn["candidates"].size(), 1u);
  CHECK_STR(learn["candidates"][0]["code"] | "", "0xE5A3C1");
  CHECK(learn["candidates"][0]["reliable"] | false);
  int pulse = learn["candidates"][0]["pulseUs"] | 0;
  CHECK_NEAR(pulse, 350, 15);
  // commit as device with one button bound to an action
  char body[512];
  std::snprintf(body, sizeof(body), R"({"name":"Taster Flur","protocol":"ev1527","pulseUs":%d,
     "buttons":[{"index":1,"name":"Plus","code":"0xE5A3C1",
       "bindings":[{"gesture":"short","actions":[{"type":"adjust_setpoint","room":"wohnen","delta":0.5}]}]}]})", pulse);
  auto add = call(api, "POST", "/api/rf/devices", body);
  CHECK_EQ(add.status, 200);
  int id = parse(add)["id"] | 0;
  CHECK(id > 0);
  CHECK_EQ(r.sys.rfRx.unknownCodes().size(), 0u);  // learned codes leave the unknown list
  r.run(11000);
  float sp = r.sys.heating.room("wohnen")->setpoint;
  r.sendRf(0xE5A3C1);
  r.run(11000);
  CHECK_NEAR(r.sys.heating.room("wohnen")->setpoint, sp + 0.5f, 0.01);
  // change
  std::string path = "/api/rf/devices/" + std::to_string(id);
  std::snprintf(body, sizeof(body), R"({"name":"Taster Bad","protocol":"ev1527","pulseUs":%d,"enabled":false,
     "buttons":[{"index":1,"name":"Plus","code":"0xE5A3C1"}]})", pulse);
  CHECK_EQ(call(api, "PUT", path.c_str(), body).status, 200);
  auto list = parse(call(api, "GET", "/api/rf/devices"));
  CHECK_STR(list["devices"][0]["name"] | "", "Taster Bad");
  CHECK_EQ(list["devices"][0]["stats"]["presses"] | 0, 1);  // statistics survive the edit
  // invalid: binding to a heating circuit output is rejected
  auto bad = call(api, "PUT", path.c_str(), R"({"name":"X","buttons":[{"index":1,"code":"0xE5A3C1",
     "bindings":[{"gesture":"short","actions":[{"type":"set_output","output":1,"state":true}]}]}]})");
  CHECK_EQ(bad.status, 400);
  // remove
  CHECK_EQ(call(api, "DELETE", path.c_str()).status, 200);
  CHECK_EQ(parse(call(api, "GET", "/api/rf/devices"))["devices"].size(), 0u);
}

TEST(api_rf_ignore_and_monitor) {
  Rig r;
  r.basicRoom();
  ApiRouter api(r.sys);
  r.sendRf(0x0BADC0);
  auto mon = parse(call(api, "GET", "/api/rf/monitor", "", {{"since", "0"}}));
  CHECK(mon["frames"].size() >= 5);
  CHECK_STR(mon["frames"][0]["status"] | "", "unknown");
  CHECK_EQ(call(api, "POST", "/api/rf/ignored", R"({"code":"0x0BADC0","note":"Nachbar"})").status, 200);
  uint32_t seq = mon["seq"] | 0u;
  r.sendRf(0x0BADC0);
  auto mon2 = parse(call(api, "GET", "/api/rf/monitor", "", {{"since", std::to_string(seq)}}));
  CHECK_STR(mon2["frames"][0]["status"] | "", "ignored");
  CHECK_EQ(call(api, "DELETE", "/api/rf/ignored/0x0BADC0").status, 200);
}

TEST(api_rf_tx_device_and_send) {
  Rig r;
  r.basicRoom();
  ApiRouter api(r.sys);
  auto add = call(api, "POST", "/api/rf/tx/devices",
                  R"({"name":"Funksteckdose","group":"licht","pulseUs":320,"repeats":6,
                      "commands":[{"name":"an","code":"0x15A5A1"},{"name":"aus","code":"0x15A5A2","automationAllowed":false}]})");
  CHECK_EQ(add.status, 200);
  int id = parse(add)["id"] | 0;
  std::string cmd = std::string(R"({"command":")") + std::to_string(id) + "/1\"}";
  CHECK_EQ(call(api, "POST", "/api/rf/tx/send", cmd.c_str()).status, 200);
  CHECK_EQ(r.sys.rfTx.queueLength(), 1u);
  // duty cycle setting is bounded by the regulatory maximum
  CHECK_EQ(call(api, "PUT", "/api/rf/config", R"({"tx":{"dutyCyclePct":25}})").status, 400);
  CHECK_EQ(call(api, "PUT", "/api/rf/config", R"({"tx":{"dutyCyclePct":5}})").status, 200);
  auto st = parse(call(api, "GET", "/api/rf/status"));
  CHECK_NEAR(st["tx"]["dutyCyclePct"] | 0.f, 5.f, 1e-4);
}

TEST(api_display_framebuffer_and_config) {
  Rig r;
  r.basicRoom();
  ApiRouter api(r.sys);
  r.run(2000);
  auto fb = parse(call(api, "GET", "/api/display/framebuffer", "", {{"page", "2"}}));
  std::string b64 = fb["fb"] | "";
  CHECK_EQ(b64.size(), 1368u);  // 1024 bytes
  CHECK_EQ(fb["page"] | 0, 2);
  CHECK_EQ(call(api, "PUT", "/api/display/config", R"({"rotation":90})").status, 400);
  CHECK_EQ(call(api, "PUT", "/api/display/config", R"({"rotation":180,"contrast":10,"timeoutSec":60})").status, 200);
  CHECK_EQ(r.sys.display.cfg.rotation, 180);
  CHECK_EQ(call(api, "POST", "/api/display/page", R"({"page":3})").status, 200);
  CHECK_EQ(r.sys.display.page(), 2);
}

TEST(api_rules_validation_test_mode_and_catalog) {
  Rig r;
  r.basicRoom();
  ApiRouter api(r.sys);
  auto res = parse(call(api, "POST", "/api/rules", R"({"name":"Ausgang","enabled":true,
      "triggers":[{"type":"manual"}],"actions":[{"type":"set_output","output":1,"state":true}]})"));
  CHECK(!(res["enabled"] | true));
  CHECK(std::string(res["validationError"] | "").size() > 0);
  auto good = parse(call(api, "POST", "/api/rules", R"({"name":"Eco","enabled":true,"priority":60,
      "triggers":[{"type":"manual"}],"conditions":[{"type":"window","ref":"wohnen","value":false}],
      "actions":[{"type":"set_room_mode","room":"wohnen","mode":"eco"}]})"));
  CHECK(good["enabled"] | false);
  int id = good["id"] | 0;
  std::string tp = "/api/rules/" + std::to_string(id) + "/test";
  auto rep = parse(call(api, "POST", tp.c_str(), R"({"event":{"type":"manual"}})"));
  CHECK(rep["wouldRun"] | false);
  CHECK(r.sys.heating.room("wohnen")->mode == RoomMode::Comfort);  // test mode changes nothing
  std::string rp = "/api/rules/" + std::to_string(id) + "/run";
  CHECK_EQ(call(api, "POST", rp.c_str()).status, 200);
  r.run(200, false);
  CHECK(r.sys.heating.room("wohnen")->mode == RoomMode::Eco);
  auto cat = parse(call(api, "GET", "/api/automation/catalog"));
  CHECK(cat["triggers"].size() >= 12);
  CHECK(cat["actions"].size() >= 20);
  CHECK(cat["refs"]["rooms"].size() == 1);
  auto log = parse(call(api, "GET", "/api/automation/log"));
  CHECK(log["log"].size() >= 2);
}

TEST(api_config_export_import_and_secrets) {
  Rig a;
  a.basicRoom();
  ApiRouter api(a.sys);
  CHECK_EQ(call(api, "PUT", "/api/config/mqtt", R"({"enabled":true,"host":"10.0.0.2","password":"geheim"})").status, 200);
  auto m = parse(call(api, "GET", "/api/config/mqtt"));
  CHECK_STR(m["password"] | "", "********");  // never returned
  CHECK_EQ(call(api, "PUT", "/api/config/mqtt", R"({"host":"10.0.0.3","password":"********"})").status, 200);
  CHECK_STR(a.sys.mqttCfg.password, "geheim");  // mask keeps the old secret
  auto exp = call(api, "GET", "/api/config/export");
  Rig b;
  ApiRouter api2(b.sys);
  CHECK_EQ(call(api2, "POST", "/api/config/import", exp.body.c_str()).status, 200);
  CHECK(b.sys.heating.room("wohnen") != nullptr);
  CHECK_STR(b.sys.mqttCfg.host, "10.0.0.3");
  CHECK_EQ(call(api, "GET", "/api/nope").status, 404);
  CHECK_EQ(call(api, "POST", "/api/rules", "{kaputt").status, 400);
}

TEST(api_status_and_hardware_without_platform) {
  Rig r;
  r.basicRoom();
  ApiRouter api(r.sys);
  r.run(2000);
  auto s = parse(call(api, "GET", "/api/status"));
  CHECK_STR(s["firmware"] | "", kFirmwareVersion);
  CHECK_EQ(s["rooms"].size(), 1u);
  auto h = call(api, "GET", "/api/hardware");
  CHECK_EQ(h.status, 200);
  CHECK_EQ(call(api, "POST", "/api/system/reboot").status, 501);
}

TEST(mqtt_discovery_entities_and_permissions) {
  Rig r;
  r.basicRoom();
  r.sys.platform.deviceId = "a1b2c3";
  r.load(ConfigSection::Io, R"({"outputs":[{"n":7,"name":"Zirkulation","function":"general","haAllowed":true}]})");
  r.load(ConfigSection::Rf, R"({"rx":[{"id":1,"name":"Fenster","function":"window","room":"wohnen",
     "buttons":[{"index":1,"name":"auf","code":"0xA1B2C3","role":"state_on"}]},
     {"id":2,"name":"FB","buttons":[{"index":1,"name":"A","code":"0x101010"}]}],
     "tx":[{"id":1,"name":"Steckdose","commands":[{"id":1,"name":"an","code":"0x222221","haExposed":true},
                                               {"id":2,"name":"aus","code":"0x222222","haExposed":false}]}]})");
  MqttBridge br(r.sys);
  std::vector<MqttMsg> msgs;
  br.discovery(msgs);
  auto has = [&](const std::string& part) {
    for (auto& m : msgs)
      if (m.topic.find(part) != std::string::npos && !m.payload.empty()) return true;
    return false;
  };
  CHECK(has("climate/heatcore_a1b2c3/room_wohnen/config"));
  CHECK(has("binary_sensor/heatcore_a1b2c3/rf_1/config"));
  CHECK(has("event/heatcore_a1b2c3/rf_2_event/config"));
  CHECK(has("button/heatcore_a1b2c3/rf_tx_1_1/config"));
  CHECK(!has("rf_tx_1_2"));            // not exposed
  CHECK(has("switch/heatcore_a1b2c3/output_7/config"));
  CHECK(!has("switch/heatcore_a1b2c3/output_1/config"));  // heating circuit never as switch
  CHECK(has("select/heatcore_a1b2c3/display_page/config"));
  CHECK(has("sensor/heatcore_a1b2c3/sensor_ow1_01/config"));
  // commands
  std::string base = r.sys.baseTopic();
  std::string err;
  CHECK(br.handle(base + "/room/wohnen/setpoint/set", "22.5", &err));
  CHECK_STR(err, "");
  CHECK(r.sys.heating.room("wohnen")->manualActive);
  CHECK_NEAR(r.sys.heating.room("wohnen")->manualSetpoint, 22.5, 0.01);
  CHECK(br.handle(base + "/room/wohnen/setpoint/set", "99", &err));  // clamped to max
  CHECK_NEAR(r.sys.heating.room("wohnen")->manualSetpoint, r.sys.safety.cfg.maxSetpoint, 0.01);
  CHECK(br.handle(base + "/output/1/set", "ON", &err));
  CHECK(!err.empty());  // circuit output not switchable by HA
  CHECK(br.handle(base + "/rf/tx/1/2/send", "PRESS", &err));
  CHECK(!err.empty());  // not exposed for HA
  CHECK(br.handle(base + "/rf/tx/1/1/send", "PRESS", &err));
  CHECK_STR(err, "");
  CHECK(br.handle(base + "/display/page/set", "circuits", &err));
  CHECK_EQ(r.sys.display.page(), 2);
  CHECK(br.handle(base + "/mode/set", "vacation", &err));
  CHECK(r.sys.heating.globalMode == GlobalMode::Vacation);
  CHECK(!br.handle(base + "/safety/maxFlowTemp/set", "90", &err));  // no such command
  CHECK(!br.handle("other/topic", "x", &err));
  // removing a device deletes its discovery config
  r.sys.rfReg.removeRx(2);
  std::vector<MqttMsg> again;
  br.discovery(again);
  bool removed = false;
  for (auto& m : again)
    if (m.topic.find("rf_2_event") != std::string::npos && m.payload.empty()) removed = true;
  CHECK(removed);
  // state + events
  std::vector<MqttMsg> st;
  br.state(st);
  bool roomState = false;
  for (auto& m : st)
    if (m.topic == base + "/room/wohnen/state") roomState = m.payload.find("\"setpoint\"") != std::string::npos;
  CHECK(roomState);
}

TEST(mqtt_generic_inbound_triggers_rule) {
  Rig r;
  r.basicRoom();
  r.load(ConfigSection::Rules, R"({"rules":[{"id":1,"name":"HA Eco","enabled":true,"cooldownSec":0,
      "triggers":[{"type":"mqtt","topic":"eco","payload":"on"}],"actions":[{"type":"set_global_mode","mode":"eco"}]}]})");
  MqttBridge br(r.sys);
  CHECK(br.handle(r.sys.baseTopic() + "/in/eco", "on"));
  r.run(200, false);
  CHECK(r.sys.heating.globalMode == GlobalMode::Eco);
}
