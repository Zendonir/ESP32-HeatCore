// Bluetooth thermometers in the system: discovery, adoption, keys, Home Assistant.
#include <vector>

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
JsonDocument parse(const ApiResponse& r) {
  JsonDocument d;
  deserializeJson(d, r.body);
  return d;
}

ble::Mac mac(const char* s) {
  ble::Mac m{};
  ble::parseMac(s, m);
  return m;
}

// pvvx custom format advertisement
std::vector<uint8_t> pvvx(const ble::Mac& m, float t, float h, int bat, uint8_t counter) {
  int16_t ti = static_cast<int16_t>(t * 100.f + (t < 0 ? -0.5f : 0.5f));
  uint16_t hi = static_cast<uint16_t>(h * 100.f + 0.5f);
  std::vector<uint8_t> a = {0x02, 0x01, 0x06, 0x12, 0x16, 0x1A, 0x18};
  for (int i = 5; i >= 0; --i) a.push_back(m[static_cast<size_t>(i)]);
  a.push_back(static_cast<uint8_t>(ti & 0xFF));
  a.push_back(static_cast<uint8_t>(ti >> 8));
  a.push_back(static_cast<uint8_t>(hi & 0xFF));
  a.push_back(static_cast<uint8_t>(hi >> 8));
  a.push_back(0xB8);
  a.push_back(0x0B);
  a.push_back(static_cast<uint8_t>(bat));
  a.push_back(counter);
  a.push_back(0x04);
  return a;
}

void send(Rig& r, const ble::Mac& m, const std::vector<uint8_t>& a, int rssi = -70) {
  r.sys.ble.ingest(m, rssi, a.data(), a.size(), r.clock.ms);
}

}  // namespace

TEST(ble_hub_discover_adopt_and_rate_limit) {
  Rig r;
  r.basicRoom();
  ble::Mac m = mac("A4:C1:38:11:22:33");
  // disabled: nothing happens
  send(r, m, pvvx(m, 21.5f, 50.f, 80, 1));
  CHECK(r.sys.ble.devices().empty());
  r.load(ConfigSection::Sensors, R"({"sensors":[{"id":"ow1:01","name":"Fuehler","room":"wohnen"}],"ble":{"enabled":true}})");
  send(r, m, pvvx(m, 21.5f, 50.f, 80, 1));
  CHECK_EQ(r.sys.ble.devices().size(), 1u);
  bool found = false;
  for (auto& d : r.sys.sensors.discovered())
    if (d.id == "ble:A4C138112233") found = d.kind == SensorKind::Ble && d.batteryPct == 80 && d.hasHumidity;
  CHECK(found);

  ApiRouter api(r.sys);
  CHECK_EQ(call(api, "POST", "/api/sensors/adopt", R"({"id":"ble:A4C138112233","name":"Bad","room":"wohnen"})").status,
           200);
  const Sensor* s = r.sys.sensors.find("ble:A4C138112233");
  CHECK(s && s->available());
  CHECK_EQ(s->timeoutSec, 900u);
  CHECK_NEAR(s->value, 21.5f, 0.01f);
  CHECK_EQ(s->batteryPct, 80);

  // repeated message and values within 30 s are not forwarded (no MQTT flood)
  uint32_t upd = s->updates;
  r.clock.advance(2000);
  send(r, m, pvvx(m, 21.5f, 50.f, 80, 1));
  send(r, m, pvvx(m, 21.6f, 50.f, 80, 2));
  CHECK_EQ(s->updates, upd);
  r.clock.advance(30000);
  send(r, m, pvvx(m, 21.7f, 49.f, 79, 3));
  CHECK_EQ(s->updates, upd + 1);
  CHECK_NEAR(s->value, 21.7f, 0.01f);
  CHECK_NEAR(s->humidity, 49.f, 0.01f);
  // a new value inside the interval, then only repeats of it: delivered when the interval expires
  r.clock.advance(5000);
  send(r, m, pvvx(m, 22.4f, 48.f, 79, 4));
  CHECK_NEAR(s->value, 21.7f, 0.01f);
  for (int i = 0; i < 30; ++i) {
    r.run(1000, false);
    send(r, m, pvvx(m, 22.4f, 48.f, 79, 4));
  }
  CHECK_EQ(s->updates, upd + 2);
  CHECK_NEAR(s->value, 22.4f, 0.01f);

  auto st = parse(call(api, "GET", "/api/ble"));
  CHECK(st["enabled"] | false);
  CHECK_EQ(st["devices"].size(), 1u);
  CHECK_STR(st["devices"][0]["format"] | "", "pvvx");
  CHECK(st["devices"][0]["adopted"] | false);
  CHECK_EQ(st["devices"][0]["rssi"].as<int>(), -70);

  // the room uses it like any other sensor; timeout after 15 min without reception
  r.run(16u * 60u * 1000u, false);
  CHECK(!s->available());
}

TEST(ble_hub_encrypted_device_needs_key) {
  Rig r;
  r.load(ConfigSection::Sensors, R"({"sensors":[],"ble":{"enabled":true}})");
  // Xiaomi LYWSD03MMC stock firmware, MiBeacon v5 (see test_ble.cpp)
  const std::vector<uint8_t> adv = {0x02, 0x01, 0x06, 0x1A, 0x16, 0x95, 0xFE, 0x58, 0x50, 0x5B, 0x05,
                                    0x37, 0x3C, 0x2B, 0x1A, 0x38, 0xC1, 0xA4, 0x13, 0xED, 0x9E, 0x75,
                                    0xD6, 0x01, 0x02, 0x03, 0x4C, 0x6A, 0x50, 0x1D};
  ble::Mac m = mac("A4:C1:38:1A:2B:3C");
  send(r, m, adv);
  ApiRouter api(r.sys);
  auto st = parse(call(api, "GET", "/api/ble"));
  CHECK(st["devices"][0]["needsKey"] | false);
  CHECK(r.sys.sensors.discovered().empty());
  // wrong format, then the right key
  CHECK_EQ(call(api, "PUT", "/api/ble/key", R"({"mac":"A4:C1:38:1A:2B:3C","key":"1234"})").status, 400);
  CHECK_EQ(call(api, "PUT", "/api/ble/key", R"({"mac":"A4:C1:38:1A:2B:3C","key":"e9efaa6873f9f9c87a5e75a5f814801c"})")
               .status,
           200);
  send(r, m, adv);
  CHECK_EQ(r.sys.sensors.discovered().size(), 1u);
  CHECK_NEAR(r.sys.sensors.discovered()[0].lastValue, 23.4f, 0.01f);
  st = parse(call(api, "GET", "/api/ble"));
  CHECK(!(st["devices"][0]["needsKey"] | false));
  CHECK(st["devices"][0]["hasKey"] | false);
  // a random advertiser address does not split the device: the MAC in the frame counts
  send(r, mac("11:22:33:44:55:66"), adv);
  CHECK_EQ(r.sys.ble.devices().size(), 1u);

  // the key is stored, but never returned by the API; the mask keeps it
  auto cfg = parse(call(api, "GET", "/api/config/export"));
  CHECK_STR(cfg["sensors"]["ble"]["keys"][0]["key"] | "", "********");
  JsonDocument full;
  r.sys.sectionToJson(ConfigSection::Sensors, full.to<JsonObject>(), true);
  CHECK_STR(full["ble"]["keys"][0]["key"] | "", "e9efaa6873f9f9c87a5e75a5f814801c");
  CHECK_EQ(call(api, "POST", "/api/config/import", cfg.as<std::string>().c_str()).status, 200);
  CHECK(r.sys.ble.keyFor(m) != nullptr);
  // removing the key
  CHECK_EQ(call(api, "PUT", "/api/ble/key", R"({"mac":"A4C1381A2B3C","key":""})").status, 200);
  CHECK(r.sys.ble.keyFor(m) == nullptr);
  CHECK(!r.sys.sectionFromJson(ConfigSection::Sensors, parse(ApiResponse{200, R"({"ble":{"keys":[{"mac":"zz","key":"00"}]}})"})
                                                           .as<JsonObjectConst>())
             .empty());
}

TEST(ble_hub_split_xiaomi_messages_and_device_list_limit) {
  Rig r;
  r.load(ConfigSection::Sensors, R"({"sensors":[{"id":"ble:4C65A8DDEEFF","name":"Kind","room":""}],"ble":{"enabled":true}})");
  ble::Mac m = mac("4C:65:A8:DD:EE:FF");
  auto mi = [&](uint8_t cnt, std::vector<uint8_t> obj) {
    std::vector<uint8_t> sd = {0x50, 0x30, 0xAA, 0x01, cnt, 0xFF, 0xEE, 0xDD, 0xA8, 0x65, 0x4C};
    sd.insert(sd.end(), obj.begin(), obj.end());
    std::vector<uint8_t> a = {static_cast<uint8_t>(sd.size() + 3), 0x16, 0x95, 0xFE};
    a.insert(a.end(), sd.begin(), sd.end());
    return a;
  };
  send(r, m, mi(1, {0x06, 0x10, 0x02, 0xE3, 0x01}));  // humidity only: no temperature yet, nothing forwarded
  const Sensor* s = r.sys.sensors.find("ble:4C65A8DDEEFF");
  CHECK(s->status == SensorStatus::Never);
  send(r, m, mi(2, {0x04, 0x10, 0x02, 0xDD, 0x00}));  // temperature
  CHECK(s->available());
  CHECK_NEAR(s->value, 22.1f, 0.01f);
  CHECK(s->hasHumidity);
  CHECK_NEAR(s->humidity, 48.3f, 0.01f);
  // only humidity and battery messages from now on: the temperature is not
  // refreshed by them, the sensor times out (room falls back)
  for (int i = 0; i < 20 * 60 / 10; ++i) {
    r.run(10000, false);
    send(r, m, mi(static_cast<uint8_t>(10 + i), {0x06, 0x10, 0x02, 0xE3, 0x01}));
  }
  CHECK(!s->available());
  send(r, m, mi(200, {0x04, 0x10, 0x02, 0xDD, 0x00}));
  r.run(1000, false);
  CHECK(s->available());
  // many devices in range: the list stays bounded, the oldest is forgotten
  for (int i = 0; i < 40; ++i) {
    r.clock.advance(10);
    ble::Mac x = mac("A4:C1:38:00:00:00");
    x[5] = static_cast<uint8_t>(i);
    send(r, x, pvvx(x, 20.f, 40.f, 90, 1));
  }
  CHECK_EQ(r.sys.ble.devices().size(), ble::BleHub::kMaxDevices);
  // devices not heard of for an hour disappear from the list
  r.run(3700u * 1000u, false);
  CHECK(r.sys.ble.devices().empty());
}

TEST(ble_hub_home_assistant_entities) {
  Rig r;
  r.sys.platform.deviceId = "a1b2c3";
  r.load(ConfigSection::Sensors, R"({"sensors":[{"id":"ble:A4C138112233","name":"Bad","room":""}],"ble":{"enabled":true}})");
  MqttBridge br(r.sys);
  std::vector<MqttMsg> before;
  br.discovery(before);
  uint32_t rev = r.sys.sensors.revision();
  ble::Mac m = mac("A4:C1:38:11:22:33");
  send(r, m, pvvx(m, 19.25f, 55.5f, 64, 7));
  CHECK(r.sys.sensors.revision() != rev);  // humidity and battery appeared: discovery is republished
  std::vector<MqttMsg> msgs;
  br.discovery(msgs);
  auto has = [&](const std::string& part) {
    for (auto& x : msgs)
      if (x.topic.find(part) != std::string::npos && !x.payload.empty()) return true;
    return false;
  };
  CHECK(has("/sensor_ble_A4C138112233/config"));
  CHECK(has("_h/config"));
  CHECK(has("_batpct/config"));
  CHECK(has("_bat/config"));
  std::vector<MqttMsg> state;
  br.state(state);
  bool ok = false;
  for (auto& x : state)
    if (x.topic.find("/sensor/ble_A4C138112233/state") != std::string::npos)
      ok = x.payload.find("\"bat\":64") != std::string::npos && x.payload.find("\"h\":55.5") != std::string::npos;
  CHECK(ok);
}
