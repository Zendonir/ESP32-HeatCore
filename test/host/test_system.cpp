#include "SimRig.h"

using namespace hc;
using sim::Rig;

TEST(system_boot_outputs_safe_and_off) {
  Rig r;
  r.basicRoom();
  // no sensor value yet: nothing may switch on
  r.run(3000, false);
  CHECK_EQ(r.sys.outputMask(), 0u);
  // NO valve: the safe (closed) state is output ON - documented exception
  r.load(ConfigSection::Heating, R"({"rooms":[{"id":"wohnen","name":"W","mode":"comfort"}],
     "circuits":[{"n":2,"room":"wohnen","valveNormallyOpen":true}]})");
  CHECK_EQ(r.sys.safeOutputMask() & 0x2, 0x2);
}

TEST(system_hysteresis_and_valve_min_times) {
  Rig r;
  r.basicRoom();
  r.plants["wohnen"].T = 19.f;
  r.run(70000);
  CHECK(r.output(1));  // 19 < 21 - 0.2
  CHECK(r.sys.heating.room("wohnen")->demand);
  // jump above setpoint: must stay open until minOnSec (60 s) elapsed
  r.plants["wohnen"].T = 23.f;
  r.sys.sensors.update("ow1:01", 23.f, r.clock.ms);
  r.run(11000, false);
  CHECK(!r.sys.heating.room("wohnen")->demand);
  r.run(70000, false);
  CHECK(!r.output(1));
}

TEST(system_rf_window_contact_pauses_room) {
  Rig r;
  r.basicRoom();
  r.load(ConfigSection::Rf, R"({"rx":[{"id":1,"name":"Fenster","function":"window","room":"wohnen",
     "buttons":[{"index":1,"name":"auf","code":"0xA1B2C3","role":"state_on"},
                {"index":2,"name":"zu","code":"0xA1B2C4","role":"state_off"}]}]})");
  r.plants["wohnen"].T = 19.f;
  r.run(70000);
  CHECK(r.output(1));
  r.sendRf(0xA1B2C3);
  CHECK(r.sys.heating.room("wohnen")->windowOpen());
  r.run(80000);
  CHECK(!r.output(1));
  CHECK_STR(r.sys.heating.room("wohnen")->reason, "Fenster offen");
  r.sendRf(0xA1B2C4);
  CHECK(!r.sys.heating.room("wohnen")->windowOpen());
  r.run(80000);
  CHECK(r.output(1));
}

TEST(system_rf_button_binding_adjusts_setpoint) {
  Rig r;
  r.basicRoom();
  r.load(ConfigSection::Rf, R"({"rx":[{"id":3,"name":"FB",
     "buttons":[{"index":1,"name":"plus","code":"0x123451",
       "bindings":[{"gesture":"short","actions":[{"type":"adjust_setpoint","room":"wohnen","delta":0.5}]}]}]}]})");
  r.run(11000);
  float before = r.sys.heating.room("wohnen")->setpoint;
  r.sendRf(0x123451);
  r.run(11000);
  CHECK_NEAR(r.sys.heating.room("wohnen")->setpoint, before + 0.5f, 0.01);
  CHECK_STR(r.sys.heating.room("wohnen")->reason, "manuell");
  // unknown code next to it: no change
  r.sendRf(0x123452);
  r.run(11000);
  CHECK_NEAR(r.sys.heating.room("wohnen")->setpoint, before + 0.5f, 0.01);
  CHECK_EQ(r.sys.rfRx.unknownCodes().size(), 1u);
}

TEST(system_rf_binding_cannot_switch_heating_circuit) {
  Rig r;
  r.basicRoom();
  JsonDocument d;
  deserializeJson(d, R"({"rx":[{"id":3,"name":"FB","buttons":[{"index":1,"name":"x","code":"0x123451",
     "bindings":[{"gesture":"short","actions":[{"type":"set_output","output":1,"state":true}]}]}]}]})");
  std::string err = r.sys.sectionFromJson(ConfigSection::Rf, d.as<JsonObjectConst>());
  CHECK(!err.empty());  // circuit output may not be switched by RF codes
}

TEST(system_di_and_rf_share_automation_trigger) {
  Rig r;
  r.basicRoom();
  r.load(ConfigSection::Io, R"({"inputs":[{"n":3,"name":"Taster","kind":"button"}]})");
  r.load(ConfigSection::Rf, R"({"rx":[{"id":7,"name":"FB","buttons":[{"index":1,"name":"a","code":"0x777771"}]}]})");
  r.load(ConfigSection::Rules, R"({"rules":[
    {"id":1,"name":"Eco per Taster","enabled":true,"priority":50,"cooldownSec":0,
     "triggers":[{"type":"input","ref":"di:3","gesture":"short"},{"type":"input","ref":"rf:7/1","gesture":"short"}],
     "actions":[{"type":"set_global_mode","mode":"eco"}]},
    {"id":2,"name":"Normal","enabled":true,"priority":50,"cooldownSec":0,
     "triggers":[{"type":"input","ref":"rf:7/1","gesture":"long"}],
     "actions":[{"type":"set_global_mode","mode":"normal"}]}]})");
  CHECK(r.sys.automation.rule(1)->valid);
  r.setInputs(0x0000);  // resting state at boot
  r.setInputs(0x0004);
  r.setInputs(0x0000);
  r.run(200, false);
  CHECK(r.sys.heating.globalMode == GlobalMode::Eco);
  r.sys.heating.setGlobalMode(GlobalMode::Normal, 0, r.clock.ms, 0, 0, EventSource::Web);
  r.sendRf(0x777771);
  CHECK(r.sys.heating.globalMode == GlobalMode::Eco);
  CHECK_EQ(r.sys.automation.rule(1)->execCount, 2u);
}

TEST(system_automation_priority_conflict) {
  Rig r;
  r.basicRoom();
  r.load(ConfigSection::Rules, R"({"rules":[
    {"id":1,"name":"Niedrig","enabled":true,"priority":10,"cooldownSec":0,
     "triggers":[{"type":"manual"}],"actions":[{"type":"set_setpoint","room":"wohnen","value":19}]},
    {"id":2,"name":"Hoch","enabled":true,"priority":90,"cooldownSec":0,
     "triggers":[{"type":"global_mode","state":"away"}],"actions":[{"type":"set_setpoint","room":"wohnen","value":17}]},
    {"id":3,"name":"Mittel","enabled":true,"priority":50,"cooldownSec":0,
     "triggers":[{"type":"global_mode","state":"away"}],"actions":[{"type":"set_setpoint","room":"wohnen","value":23}]}]})");
  r.sys.heating.setGlobalMode(GlobalMode::Away, 0, r.clock.ms, 0, 0, EventSource::Web);
  r.run(500, false);
  CHECK(r.sys.heating.room("wohnen")->manualActive);
  CHECK_NEAR(r.sys.heating.room("wohnen")->manualSetpoint, 17.f, 0.01);
  CHECK(r.sys.automation.stats().conflicts >= 1);
  CHECK_STR(r.sys.runRule(1, EventSource::Web), "");
  r.run(200, false);
  CHECK_NEAR(r.sys.heating.room("wohnen")->manualSetpoint, 19.f, 0.01);
}

TEST(system_automation_validation_and_safety_limits) {
  Rig r;
  r.basicRoom();
  Rule rule;
  rule.name = "boese";
  rule.enabled = true;
  Trigger t;
  t.type = TriggerType::Manual;
  rule.triggers.push_back(t);
  Action a;
  a.type = ActionType::SetOutput;  // output 1 is a heating circuit
  a.index = 1;
  a.value = 1;
  rule.actions.push_back(a);
  std::string verr;
  uint16_t id = 0;
  CHECK_STR(r.sys.automation.upsert(rule, &id, &verr), "");
  CHECK(!verr.empty());
  CHECK(!r.sys.automation.rule(id)->enabled);  // stored but not activated
  // system alarms are protected, setpoints are bounded
  rule.actions[0] = Action{ActionType::ClearAlarm, "", "safety.flow"};
  CHECK(!r.sys.automation.validate(rule).empty());
  rule.actions[0] = Action{ActionType::SetSetpoint, "wohnen", "", "", 35.f};
  CHECK(!r.sys.automation.validate(rule).empty());
  rule.actions[0] = Action{ActionType::SetSetpoint, "wohnen", "", "", 22.f};
  CHECK_STR(r.sys.automation.validate(rule), "");
}

TEST(system_automation_static_loop_detection) {
  Rig r;
  r.basicRoom();
  r.load(ConfigSection::Rules, R"({"rules":[
    {"id":1,"name":"A","enabled":true,"triggers":[{"type":"room_state","ref":"wohnen","state":"setpoint"}],
     "actions":[{"type":"set_room_mode","room":"wohnen","mode":"eco"}]}]})");
  CHECK(r.sys.automation.rule(1)->enabled);
  Rule b;
  b.name = "B";
  b.enabled = true;
  Trigger t;
  t.type = TriggerType::RoomState;
  t.ref = "wohnen";
  t.state = "mode";
  b.triggers.push_back(t);
  b.actions.push_back(Action{ActionType::AdjustSetpoint, "wohnen", "", "", 0.5f});
  std::string err = r.sys.automation.validate(b);
  CHECK(err.find("Endlosschleife") != std::string::npos);
}

TEST(system_automation_runtime_depth_limit) {
  Rig r;
  r.basicRoom();
  r.load(ConfigSection::Heating, R"({"rooms":[{"id":"a","name":"A"},{"id":"b","name":"B"},{"id":"c","name":"C"},{"id":"d","name":"D"}]})");
  // chain a -> b -> c -> d: the 4th hop exceeds maxDepth 3
  r.load(ConfigSection::Rules, R"({"rules":[
    {"id":1,"name":"ab","enabled":true,"cooldownSec":0,"triggers":[{"type":"room_state","ref":"a","state":"mode"}],"actions":[{"type":"set_room_mode","room":"b","mode":"eco"}]},
    {"id":2,"name":"bc","enabled":true,"cooldownSec":0,"triggers":[{"type":"room_state","ref":"b","state":"mode"}],"actions":[{"type":"set_room_mode","room":"c","mode":"eco"}]},
    {"id":3,"name":"cd","enabled":true,"cooldownSec":0,"triggers":[{"type":"room_state","ref":"c","state":"mode"}],"actions":[{"type":"set_room_mode","room":"d","mode":"eco"}]},
    {"id":4,"name":"de","enabled":true,"cooldownSec":0,"triggers":[{"type":"room_state","ref":"d","state":"mode"}],"actions":[{"type":"notify","message":"tief"}]}]})");
  Action a{ActionType::SetRoomMode, "a", "", "frost"};
  r.sys.execute(a, ActionOrigin{EventSource::Web, 0, 0, false});
  r.run(500, false);
  CHECK(r.sys.heating.room("d")->mode == RoomMode::Eco);
  CHECK_EQ(r.sys.automation.rule(4)->execCount, 0u);
  CHECK(r.sys.automation.stats().blockedDepth >= 1);
}

TEST(system_flow_overtemp_closes_all_even_manual) {
  Rig r;
  r.basicRoom();
  r.load(ConfigSection::Sensors, R"({"sensors":[{"id":"ow1:01","room":"wohnen"},{"id":"ow2:99","role":"flow"}]})");
  r.plants["wohnen"].T = 18.f;
  r.sys.sensors.update("ow2:99", 40.f, r.clock.ms);
  r.run(70000);
  CHECK(r.output(1));
  CHECK_STR(r.sys.manualOutput(1, true, 600, EventSource::Web), "");
  r.sys.sensors.update("ow2:99", 60.f, r.clock.ms);
  r.run(11000);
  CHECK(r.sys.safety.flowTripped());
  CHECK(!r.output(1));
  CHECK(r.sys.alarms.active("safety.flow"));
  r.sys.sensors.update("ow2:99", 45.f, r.clock.ms);
  r.run(11000);
  CHECK(!r.sys.safety.flowTripped());
}

TEST(system_manual_override_never_unlimited_and_mqtt_restricted) {
  Rig r;
  r.basicRoom();
  r.load(ConfigSection::Io, R"({"outputs":[{"n":5,"name":"Pumpe","function":"general","automationAllowed":false,"haAllowed":true}]})");
  CHECK(!r.sys.manualOutput(5, true, 0, EventSource::Mqtt).empty());  // HA cannot use manual mode
  CHECK_STR(r.sys.requestOutput(5, true, 0, EventSource::Mqtt), "");
  r.run(1000, false);
  CHECK(r.output(5));
  r.run((r.sys.safety.cfg.haMaxOutputSec + 5) * 1000u, false);
  CHECK(!r.output(5));  // HA request expires automatically
  // automation not allowed on this output
  Action a{ActionType::SetOutput, "", "", "", 1.f, 5, 0};
  CHECK(!r.sys.execute(a, ActionOrigin{EventSource::Automation, 1, 1, false}).empty());
}

TEST(system_sensor_loss_detected_and_circuit_closed) {
  Rig r;
  r.basicRoom();
  r.plants["wohnen"].T = 18.f;
  r.run(70000);
  CHECK(r.output(1));
  // sensor stops reporting
  r.run(6 * 60 * 1000, false);
  const Room* room = r.sys.heating.room("wohnen");
  CHECK(room->sensorFail);
  CHECK(!r.output(1));
  CHECK(r.sys.alarms.active("room.sensor.wohnen"));
  CHECK(r.sys.sensors.find("ow1:01")->status == SensorStatus::Stale);
}

TEST(system_fallback_sensor_is_used) {
  Rig r;
  r.basicRoom();
  r.load(ConfigSection::Sensors, R"({"sensors":[{"id":"ow1:01","room":"wohnen","timeoutSec":120,"fallbackId":"rf:nexus_th:5:1"},
                                         {"id":"rf:nexus_th:5:1","room":"flur","timeoutSec":900}]})");
  r.sys.sensors.update("rf:nexus_th:5:1", 18.5f, r.clock.ms);
  r.run(11000, false);
  const Room* room = r.sys.heating.room("wohnen");
  CHECK(room->hasTemp);
  CHECK(room->tempFallback);
  CHECK_NEAR(room->temperature, 18.5, 0.01);
}

TEST(system_rf_temperature_sensor_discovery) {
  Rig r;
  r.basicRoom();
  r.sys.rfSensors.setEnabled("nexus_th", true);
  auto row = rf::NexusThDecoder::encodeRow(42, true, 1, 20.7f, 50);
  for (int i = 0; i < 6; ++i) r.sys.rfRx.feed(row.data(), row.size());
  r.run(100, false);
  CHECK_EQ(r.sys.sensors.discovered().size(), 1u);
  CHECK_STR(r.sys.sensors.adopt("rf:nexus_th:42:1", "Bad Funk", "wohnen", SensorRole::Room), "");
  const Sensor* s = r.sys.sensors.find("rf:nexus_th:42:1");
  CHECK(s && s->available());
  CHECK_NEAR(s->value, 20.7, 0.05);
  CHECK(s && s->hasHumidity);
  // rtl_433 via RS485
  r.sys.reportRtl433Line(R"({"model":"LaCrosse-TX141THBv2","id":201,"channel":0,"battery_ok":0,"temperature_C":4.5})");
  r.run(100, false);
  bool found = false;
  for (auto& d : r.sys.sensors.discovered()) found |= d.id == "rf:rtl433_LaCrosse-TX141THBv2:201:0";
  CHECK(found);
}

TEST(system_rf_supervision_failsafe) {
  Rig r;
  r.basicRoom();
  r.load(ConfigSection::Rf, R"({"rx":[{"id":1,"name":"Fenster","function":"window","room":"wohnen","supervisionSec":600,"failsafeOn":false,
     "buttons":[{"index":1,"name":"auf","code":"0xA1B2C3","role":"state_on"}]}]})");
  r.sendRf(0xA1B2C3);
  CHECK(r.sys.heating.room("wohnen")->windowOpen());
  r.run(601 * 1000, false);
  // failsafe "closed": heating continues, alarm raised
  CHECK(!r.sys.heating.room("wohnen")->windowOpen());
  CHECK(r.sys.alarms.active("rf.device.1"));
}

TEST(system_heat_demand_expires) {
  Rig r;
  r.basicRoom();
  r.load(ConfigSection::Rf, R"({"rx":[{"id":2,"name":"Thermostat","function":"heat_demand","room":"wohnen","supervisionSec":300,
     "buttons":[{"index":1,"name":"an","code":"0x0F0F01","role":"state_on"},{"index":2,"name":"aus","code":"0x0F0F02","role":"state_off"}]}]})");
  r.plants["wohnen"].T = 21.3f;  // inside hysteresis band
  r.run(70000);
  CHECK(!r.output(1));
  r.sendRf(0x0F0F01);
  r.run(11000);
  CHECK(r.sys.heating.room("wohnen")->externalDemand);
  CHECK(r.output(1));
  // "off" telegram lost -> demand expires after 2x supervision time
  r.run(610 * 1000);
  CHECK(!r.sys.heating.room("wohnen")->externalDemand);
}

TEST(system_display_navigation_and_render_safe) {
  Rig r;
  r.basicRoom();
  r.load(ConfigSection::Display, R"({"pages":[true,true,false,true,true],"autoCycle":false})");
  r.run(1500);
  CHECK_EQ(r.sys.display.page(), 0);
  r.sys.display.next();
  CHECK_EQ(r.sys.display.page(), 1);
  r.sys.display.next();  // page 3 disabled -> skip to 4
  CHECK_EQ(r.sys.display.page(), 3);
  DisplaySnapshot s;
  r.sys.buildDisplaySnapshot(s);
  Framebuffer fb;
  for (int p = 0; p < kNumPages; ++p) {
    r.sys.display.renderPage(static_cast<DisplayPage>(p), fb, s);
    CHECK(fb.checksum() != Framebuffer().checksum());  // something drawn
  }
  // empty snapshot must not crash
  DisplaySnapshot empty;
  for (int p = 0; p < kNumPages; ++p) r.sys.display.renderPage(static_cast<DisplayPage>(p), fb, empty);
  // displayed-room adjust via DI binding
  r.load(ConfigSection::Io, R"({"inputs":[{"n":1,"kind":"button","bindings":[{"gesture":"short","actions":[{"type":"displayed_room_adjust","delta":-0.5}]}]}]})");
  r.setInputs(0);
  r.setInputs(1);
  r.setInputs(0);
  CHECK(r.sys.heating.room("wohnen")->manualActive);
}

TEST(system_display_hw_failure_does_not_affect_control) {
  Rig r;
  r.basicRoom();
  r.sys.display.hw.present = false;
  r.sys.display.hw.errors = 1000;
  r.sys.display.cfg.enabled = false;
  r.plants["wohnen"].T = 19.f;
  r.run(70000);
  CHECK(r.output(1));
}

TEST(system_config_roundtrip_all_sections) {
  Rig a;
  a.basicRoom();
  a.load(ConfigSection::Rf, R"({"rx":[{"id":1,"name":"FB","buttons":[{"index":1,"name":"a","code":"0x111111",
      "bindings":[{"gesture":"long","actions":[{"type":"set_global_mode","mode":"vacation","durationSec":86400}]}]}]}],
      "tx":[{"id":1,"name":"Steckdose","group":"licht","commands":[{"id":1,"name":"an","code":"0x222221","haExposed":true}]}],
      "ignored":[{"code":"0x333333","note":"Nachbar"}],
      "settings":{"tx":{"dutyCyclePct":2.5},"decoders":{"nexus_th":true}}})");
  a.load(ConfigSection::Display, R"({"contrast":77,"rotation":180,"mode":"large","language":"en","pages":[true,false,true,true,true]})");
  a.load(ConfigSection::Rules, R"({"rules":[{"id":4,"name":"Abends","enabled":true,"priority":70,
      "triggers":[{"type":"time","time":"22:00","days":[0,1,2,3,4]}],
      "conditions":[{"type":"window","ref":"wohnen","value":false}],
      "actions":[{"type":"rf_send","command":"1/1"}]}]})");
  std::map<int, std::string> dump;
  for (int s = 0; s < static_cast<int>(ConfigSection::Count); ++s) {
    JsonDocument d;
    a.sys.sectionToJson(static_cast<ConfigSection>(s), d.to<JsonObject>(), true);
    serializeJson(d, dump[s]);
  }
  Rig b;
  // load in boot order
  const ConfigSection order[] = {ConfigSection::System, ConfigSection::Network, ConfigSection::Mqtt,
                                 ConfigSection::Safety, ConfigSection::Heating, ConfigSection::Sensors,
                                 ConfigSection::Io,     ConfigSection::Rf,      ConfigSection::Display,
                                 ConfigSection::Model,  ConfigSection::Rules,   ConfigSection::Ai};
  for (auto s : order) b.load(s, dump[static_cast<int>(s)].c_str());
  for (int s = 0; s < static_cast<int>(ConfigSection::Count); ++s) {
    JsonDocument d;
    b.sys.sectionToJson(static_cast<ConfigSection>(s), d.to<JsonObject>(), true);
    std::string out;
    serializeJson(d, out);
    CHECK_STR(out, dump[s]);
  }
  CHECK(b.sys.automation.rule(4)->enabled);
  CHECK_NEAR(b.sys.rfTx.config().dutyCyclePct, 2.5, 1e-4);
  CHECK(b.sys.rfSensors.isEnabled("nexus_th"));
  CHECK_EQ(b.sys.display.cfg.contrast, 77);
}

TEST(system_runs_without_network_hooks) {
  Rig r;
  r.basicRoom();
  r.load(ConfigSection::Mqtt, R"({"enabled":true,"host":"192.168.1.2"})");
  // MQTT enabled but no connection/hook: publishing actions are best effort,
  // heating control continues.
  Action a{ActionType::MqttPublish, "", "test", "x"};
  CHECK_STR(r.sys.execute(a, ActionOrigin{EventSource::Automation, 1, 1, false}), "");
  r.plants["wohnen"].T = 19.f;
  r.run(70000);
  CHECK(r.output(1));
}

TEST(system_output_write_errors_raise_alarm) {
  Rig r;
  r.basicRoom();
  for (int i = 0; i < 3; ++i) r.sys.reportOutputWrite(false);
  CHECK(r.sys.alarms.active("hw.outputs"));
  r.run(11000, false);
  CHECK(r.sys.heating.circuits[0].fault);
  r.sys.reportOutputWrite(true);
  CHECK(!r.sys.alarms.active("hw.outputs"));
}

TEST(system_rf_loopback_selftest) {
  Rig r;
  r.basicRoom();
  CHECK_STR(r.sys.startRfLoopback(), "");
  rf::TxJob job;
  std::vector<rf::Pulse> pulses;
  CHECK(r.sys.takeTxJob(job, pulses));
  // the "air": feed our own transmission into the receiver
  for (size_t i = 0; i < pulses.size(); i += 50) {
    size_t n = std::min<size_t>(50, pulses.size() - i);
    r.sys.rfRx.feed(&pulses[i], n);
  }
  r.sys.rfRx.idle();
  r.sys.completeTxJob(job, true, rf::transmissionDurationUs(pulses) / 1000, nullptr);
  r.run(1500, false);
  CHECK(r.sys.loopback.done);
  CHECK(r.sys.loopback.ok);
  // the echo must not create unknown-code entries
  CHECK_EQ(r.sys.rfRx.unknownCodes().size(), 0u);
}

TEST(io_analog_voltage_and_current_modes) {
  Rig r;
  // defaults per A16v3 block diagram: A1/A2 0..5 V (divider 1.51), A3/A4 4..20 mA (150 Ohm)
  CHECK(r.sys.io.aiCfg[0].mode == AnalogMode::Voltage);
  CHECK(r.sys.io.aiCfg[3].mode == AnalogMode::Current);
  r.sys.io.onAnalog(0, 2.0f, r.clock.ms);
  CHECK_NEAR(r.sys.io.ai[0].volts, 3.02f, 0.001f);
  r.sys.io.onAnalog(2, 1.8f, r.clock.ms);  // 1.8 V / 150 Ohm = 12 mA
  CHECK_NEAR(r.sys.io.ai[2].milliamps, 12.f, 0.001f);
  CHECK(!r.sys.io.ai[2].loopFault);
  // 4..20 mA -> 0..100 degC
  r.load(ConfigSection::Io, R"({"analog":[{"n":3,"enabled":true,"mode":"current","shuntOhm":150,"scale":6.25,"offset":-25,"unit":"C"}]})");
  r.sys.io.onAnalog(2, 1.8f, r.clock.ms);
  CHECK_NEAR(r.sys.io.ai[2].value, 50.f, 0.01f);
  // wire break and overrange are flagged
  r.sys.io.onAnalog(2, 0.0f, r.clock.ms);
  CHECK(r.sys.io.ai[2].loopFault);
  r.sys.io.onAnalog(2, 3.3f, r.clock.ms);  // 22 mA
  CHECK(r.sys.io.ai[2].loopFault);
  // switch A3 to voltage mode, A1 to current mode via the config section
  r.load(ConfigSection::Io, R"({"analog":[{"n":3,"mode":"voltage"},{"n":1,"mode":"current"}]})");
  CHECK(r.sys.io.aiCfg[2].mode == AnalogMode::Voltage);
  CHECK_STR(r.sys.io.aiCfg[2].unit, "V");
  CHECK(r.sys.io.aiCfg[0].mode == AnalogMode::Current);
  r.sys.io.onAnalog(2, 0.0f, r.clock.ms);
  CHECK(!r.sys.io.ai[2].loopFault);
  // implausible board constants are rejected, previous config stays
  JsonDocument d;
  deserializeJson(d, R"({"analog":[{"n":2,"shuntOhm":0}]})");
  CHECK(!r.sys.sectionFromJson(ConfigSection::Io, d.as<JsonObjectConst>()).empty());
  CHECK_NEAR(r.sys.io.aiCfg[1].shuntOhm, 150.f, 0.001f);
}

TEST(io_input_bank_swap) {
  Rig r;
  r.load(ConfigSection::Io, R"({"inputs":[{"n":1,"kind":"contact","debounceMs":0},{"n":9,"kind":"contact","debounceMs":0}]})");
  r.sys.io.onInputs(0x0001, r.clock.ms);  // physical bit 0 = first expander bank
  r.clock.ms += 10;
  r.sys.io.onInputs(0x0001, r.clock.ms);
  CHECK(r.sys.io.in[0].stable);
  CHECK(!r.sys.io.in[8].stable);
  r.load(ConfigSection::Io, R"({"swapInputBanks":true})");
  CHECK(r.sys.io.swapInputBanks);
  r.sys.io.onInputs(0x0001, r.clock.ms);
  r.clock.ms += 10;
  r.sys.io.onInputs(0x0001, r.clock.ms);
  CHECK(!r.sys.io.in[0].stable);
  CHECK(r.sys.io.in[8].stable);
  // setting persists through the config round trip
  JsonDocument d;
  r.sys.io.toJson(d.to<JsonObject>(), false);
  CHECK(d["swapInputBanks"].as<bool>());
}
