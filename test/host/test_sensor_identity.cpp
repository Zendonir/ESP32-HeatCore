// 1-Wire sensors are identified by their ROM code, not by the terminal.
#include "SimRig.h"
#include "hc/mqtt/MqttBridge.h"

using namespace hc;
using sim::Rig;

TEST(onewire_id_is_bus_independent_and_migrated) {
  CHECK_STR(canonicalSensorId("ow1:28dc26d8000000de"), "ow:28DC26D8000000DE");
  CHECK_STR(canonicalSensorId("ow3:28DC26D8000000DE"), "ow:28DC26D8000000DE");
  CHECK_STR(canonicalSensorId("ow:28DC26D8000000DE"), "ow:28DC26D8000000DE");
  CHECK_STR(canonicalSensorId("ow1:01"), "ow1:01");          // not a ROM code: unchanged
  CHECK_STR(canonicalSensorId("rf:nexus_th:12:1"), "rf:nexus_th:12:1");
  CHECK_STR(canonicalSensorRef("sensor:ow2:2848F0503A190100"), "sensor:ow:2848F0503A190100");

  Rig r;
  r.load(ConfigSection::Heating, R"({"rooms":[{"id":"bad","name":"Bad","comfort":22,"eco":18,"frost":7,"mode":"comfort"},
                                              {"id":"flur","name":"Flur","comfort":20,"eco":18,"frost":7,"mode":"comfort"}],
                                     "circuits":[{"n":1,"room":"bad"}]})");
  // the same sensor was set up on terminal 1 and later found again on terminal 3
  r.load(ConfigSection::Sensors, R"({"sensors":[
      {"id":"ow1:28DC26D8000000DE","name":"Bad","room":"bad","offset":-0.3,"fallbackId":"ow2:2848F0503A190100"},
      {"id":"ow3:28DC26D8000000DE","name":"ow3:28DC26D8000000DE","room":""},
      {"id":"ow2:2848F0503A190100","name":"Flur","room":"flur"}]})");
  CHECK_EQ(r.sys.sensors.sensors().size(), 2u);
  const Sensor* s = r.sys.sensors.find("ow:28DC26D8000000DE");
  CHECK(s != nullptr);
  CHECK_STR(s->name, "Bad");
  CHECK_STR(s->room, "bad");
  CHECK_NEAR(s->offset, -0.3f, 0.001f);  // one correction per sensor
  CHECK_STR(s->fallbackId, "ow:2848F0503A190100");
  // old ids still resolve (API, MQTT, rules of earlier versions)
  CHECK(r.sys.sensors.find("ow3:28DC26D8000000DE") == s);

  // readings arrive with the bus-free id, whichever terminal the sensor is on
  r.sys.sensors.update("ow:28DC26D8000000DE", 21.3f, r.clock.ms);
  CHECK(s->available());
  CHECK_NEAR(s->value, 21.0f, 0.001f);
  CHECK(r.sys.sensors.roomTemperature("bad").valid);
  // a reading delivered with an old id lands on the same sensor
  r.sys.sensors.update("ow1:28DC26D8000000DE", 21.5f, r.clock.ms);
  CHECK_NEAR(s->value, 21.2f, 0.001f);
  CHECK(r.sys.sensors.discovered().empty());
  // calibration against a reference given with its old id
  r.sys.sensors.update("ow:2848F0503A190100", 20.5f, r.clock.ms);
  std::vector<SensorManager::CalibrationResult> res;
  CHECK_STR(r.sys.sensors.calibrate({"ow1:28DC26D8000000DE", "ow2:2848F0503A190100"}, "ow2:2848F0503A190100", 0.f, res),
            "");
  CHECK_NEAR(s->offset, -1.0f, 0.001f);  // raw 21.5 -> 20.5

  // the reverse order of the duplicates gives the same result
  r.load(ConfigSection::Sensors, R"({"sensors":[
      {"id":"ow3:28DC26D8000000DE","name":"ow3:28DC26D8000000DE","room":""},
      {"id":"ow1:28DC26D8000000DE","name":"Bad","room":"bad","offset":-0.3}]})");
  CHECK_EQ(r.sys.sensors.sensors().size(), 1u);
  CHECK_STR(r.sys.sensors.sensors()[0].name, "Bad");
  CHECK_NEAR(r.sys.sensors.sensors()[0].offset, -0.3f, 0.001f);
  // two automatically adopted entries (name = old id): no old id left as name
  r.load(ConfigSection::Sensors, R"({"sensors":[
      {"id":"ow1:28DC26D8000000DE","name":"ow1:28DC26D8000000DE"},
      {"id":"ow3:28DC26D8000000DE","name":"ow3:28DC26D8000000DE"},
      {"id":"ow2:2848F0503A190100","name":"ow2:2848F0503A190100"}]})");
  CHECK_EQ(r.sys.sensors.sensors().size(), 2u);
  for (auto& x : r.sys.sensors.sensors()) CHECK_STR(x.name, x.id);

  // safety and rules referring to old ids are converted
  r.load(ConfigSection::Safety, R"({"flowSensorId":"ow2:2848F0503A190100"})");
  CHECK_STR(r.sys.safety.cfg.flowSensorId, "ow:2848F0503A190100");
  r.load(ConfigSection::Sensors, R"({"sensors":[{"id":"ow1:28DC26D8000000DE","name":"Bad","room":"bad"}]})");
  r.load(ConfigSection::Rules, R"({"rules":[{"id":1,"name":"warm","enabled":true,
      "triggers":[{"type":"temp_threshold","ref":"sensor:ow3:28DC26D8000000DE","value":25,"direction":"above"}],
      "actions":[{"type":"set_global_mode","mode":"eco"}]}]})");
  JsonDocument d;
  r.sys.sectionToJson(ConfigSection::Rules, d.to<JsonObject>(), false);
  CHECK_STR(d["rules"][0]["triggers"][0]["ref"] | "", "sensor:ow:28DC26D8000000DE");
  // config is saved with the new ids
  JsonDocument sd;
  r.sys.sectionToJson(ConfigSection::Sensors, sd.to<JsonObject>(), false);
  CHECK_STR(sd["sensors"][0]["id"] | "", "ow:28DC26D8000000DE");
}

TEST(onewire_legacy_home_assistant_entities_removed) {
  Rig r;
  r.sys.platform.deviceId = "a1b2c3";
  r.load(ConfigSection::Sensors, R"({"sensors":[{"id":"ow1:28DC26D8000000DE","name":"Bad"}]})");
  MqttBridge br(r.sys);
  std::vector<MqttMsg> msgs;
  br.discovery(msgs);
  int cleared = 0;
  bool added = false;
  for (auto& m : msgs) {
    if (m.topic.find("/sensor_ow1_28DC26D8000000DE/config") != std::string::npos ||
        m.topic.find("/sensor_ow3_28DC26D8000000DE/config") != std::string::npos)
      cleared += m.payload.empty() ? 1 : 0;
    if (m.topic.find("/sensor_ow_28DC26D8000000DE/config") != std::string::npos && !m.payload.empty()) added = true;
  }
  CHECK_EQ(cleared, 2);
  CHECK(added);
  std::vector<MqttMsg> again;
  br.discovery(again);  // only once per start
  for (auto& m : again) CHECK(m.topic.find("sensor_ow1_") == std::string::npos);
}
