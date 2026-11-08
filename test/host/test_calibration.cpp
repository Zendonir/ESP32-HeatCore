// Sensor calibration: offsets so that sensors side by side show the same value.
#include "SimRig.h"
#include "hc/api/ApiRouter.h"

using namespace hc;
using sim::Rig;

namespace {

void fourSensors(Rig& r) {
  r.load(ConfigSection::Sensors, R"({"sensors":[
     {"id":"ow1:A","name":"A","timeoutSec":300},{"id":"ow1:B","name":"B","timeoutSec":300},
     {"id":"ow1:C","name":"C","timeoutSec":300,"offset":0.4},{"id":"ow1:D","name":"D","timeoutSec":300}]})");
  r.sys.sensors.update("ow1:A", 21.00f, r.clock.ms);
  r.sys.sensors.update("ow1:B", 21.25f, r.clock.ms);
  r.sys.sensors.update("ow1:C", 20.75f, r.clock.ms);
  r.sys.sensors.update("ow1:D", 21.50f, r.clock.ms);
}

float value(Rig& r, const char* id) { return r.sys.sensors.find(id)->value; }

ApiResponse call(ApiRouter& api, const char* method, const char* path, const char* body = "") {
  ApiRequest q;
  q.method = method;
  q.path = path;
  q.body = body;
  return api.handle(q);
}

}  // namespace

TEST(calibration_median_mean_reference_value_reset) {
  Rig r;
  fourSensors(r);
  std::vector<SensorManager::CalibrationResult> res;
  float target = 0.f;
  // median of raw 21.00, 21.25, 20.75, 21.50 = 21.125
  CHECK_STR(r.sys.sensors.calibrate({"ow1:A", "ow1:B", "ow1:C", "ow1:D"}, "median", 0.f, res, &target), "");
  CHECK_NEAR(target, 21.125f, 0.001f);
  for (const char* id : {"ow1:A", "ow1:B", "ow1:C", "ow1:D"}) CHECK_NEAR(value(r, id), 21.125f, 0.006f);
  CHECK_NEAR(res[2].oldOffset, 0.4f, 0.001f);
  // new readings keep the offsets
  r.sys.sensors.update("ow1:D", 22.5f, r.clock.ms);
  CHECK_NEAR(value(r, "ow1:D"), 22.5f - 0.375f, 0.006f);
  r.sys.sensors.update("ow1:D", 21.5f, r.clock.ms);

  // mean
  CHECK_STR(r.sys.sensors.calibrate({"ow1:A", "ow1:B"}, "mean", 0.f, res, &target), "");
  CHECK_NEAR(target, 21.125f, 0.001f);
  // reference sensor: B is the master and keeps its offset
  float bOffset = r.sys.sensors.find("ow1:B")->offset;
  CHECK_STR(r.sys.sensors.calibrate({"ow1:A", "ow1:B", "ow1:C"}, "ow1:B", 0.f, res), "");
  CHECK_NEAR(r.sys.sensors.find("ow1:B")->offset, bOffset, 0.0001f);
  CHECK_NEAR(value(r, "ow1:A"), value(r, "ow1:B"), 0.006f);
  CHECK_NEAR(value(r, "ow1:C"), value(r, "ow1:B"), 0.006f);
  // calibrated reference thermometer, a single sensor is enough
  CHECK_STR(r.sys.sensors.calibrate({"ow1:D"}, "value", 21.3f, res), "");
  CHECK_NEAR(value(r, "ow1:D"), 21.3f, 0.006f);
  // reset
  CHECK_STR(r.sys.sensors.calibrate({"ow1:A", "ow1:B", "ow1:C", "ow1:D"}, "reset", 0.f, res), "");
  CHECK_NEAR(value(r, "ow1:C"), 20.75f, 0.001f);
  CHECK_NEAR(r.sys.sensors.find("ow1:C")->offset, 0.f, 0.0001f);
}

TEST(calibration_rejects_and_changes_nothing) {
  Rig r;
  fourSensors(r);
  std::vector<SensorManager::CalibrationResult> res;
  CHECK(!r.sys.sensors.calibrate({}, "median", 0.f, res).empty());
  CHECK(!r.sys.sensors.calibrate({"ow1:A"}, "median", 0.f, res).empty());       // needs 2
  CHECK(!r.sys.sensors.calibrate({"ow1:A", "ow1:X"}, "mean", 0.f, res).empty()); // unknown
  CHECK(!r.sys.sensors.calibrate({"ow1:A", "ow1:B"}, "ow1:C", 0.f, res).empty()); // ref not selected
  CHECK(!r.sys.sensors.calibrate({"ow1:A"}, "value", 200.f, res).empty());
  // one sensor far away (outside, 5 C): offset > 10 K -> nothing changes
  r.sys.sensors.update("ow1:D", 5.f, r.clock.ms);
  std::string e = r.sys.sensors.calibrate({"ow1:A", "ow1:D"}, "value", 21.f, res);
  CHECK(e.find("gleichen Ort") != std::string::npos);
  CHECK_NEAR(r.sys.sensors.find("ow1:A")->offset, 0.f, 0.0001f);
  // sensor without a current value
  r.load(ConfigSection::Sensors, R"({"sensors":[{"id":"ow1:A","name":"A"},{"id":"ow1:E","name":"E"}]})");
  r.sys.sensors.update("ow1:A", 21.f, r.clock.ms);
  CHECK(!r.sys.sensors.calibrate({"ow1:A", "ow1:E"}, "mean", 0.f, res).empty());
}

TEST(calibration_api_persists_offsets) {
  Rig r;
  fourSensors(r);
  ApiRouter api(r.sys);
  auto res = call(api, "POST", "/api/sensors/calibrate", R"({"ids":["ow1:A","ow1:B","ow1:C","ow1:D"],"reference":"median"})");
  CHECK_EQ(res.status, 200);
  JsonDocument d;
  deserializeJson(d, res.body);
  CHECK_NEAR(d["target"].as<float>(), 21.13f, 0.006f);
  CHECK_EQ(d["results"].size(), 4u);
  JsonDocument cfg;
  r.sys.sectionToJson(ConfigSection::Sensors, cfg.to<JsonObject>(), false);
  CHECK_NEAR(cfg["sensors"][3]["offset"].as<float>(), -0.38f, 0.006f);
  auto bad = call(api, "POST", "/api/sensors/calibrate", R"({"ids":["ow1:A"],"reference":"mean"})");
  CHECK_EQ(bad.status, 400);
  // reference thermometer: value required, 0 C allowed
  float before = r.sys.sensors.find("ow1:A")->offset;
  CHECK_EQ(call(api, "POST", "/api/sensors/calibrate", R"({"ids":["ow1:A"],"reference":"value"})").status, 400);
  CHECK_EQ(call(api, "POST", "/api/sensors/calibrate", R"({"ids":["ow1:A"],"reference":"value","value":""})").status, 400);
  CHECK_NEAR(r.sys.sensors.find("ow1:A")->offset, before, 0.0001f);
  r.sys.sensors.update("ow1:A", 0.5f, r.clock.ms);  // ice water
  CHECK_EQ(call(api, "POST", "/api/sensors/calibrate", R"({"ids":["ow1:A"],"reference":"value","value":0})").status, 200);
  CHECK_NEAR(r.sys.sensors.find("ow1:A")->offset, -0.5f, 0.001f);
}
