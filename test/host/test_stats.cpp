#include <cstring>

#include "SimRig.h"
#include "hc/api/ApiRouter.h"
#include "hc/stats/History.h"

using namespace hc;
using sim::Rig;

namespace {
HistSample sample(float temp, float sp, float duty, uint8_t flags = 0, bool window = false) {
  HistSample s;
  s.tempValid = true;
  s.temp = temp;
  s.setpointValid = true;
  s.setpoint = sp;
  s.duty = duty;
  s.flags = flags;
  s.windowOpen = window;
  return s;
}

JsonDocument get(ApiRouter& api, const std::string& path, int expect = 200) {
  ApiRequest q;
  q.method = "GET";
  size_t qm = path.find('?');
  q.path = path.substr(0, qm);
  if (qm != std::string::npos) {
    std::string rest = path.substr(qm + 1);
    size_t pos = 0;
    while (pos < rest.size()) {
      size_t amp = rest.find('&', pos);
      std::string kv = rest.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
      size_t eq = kv.find('=');
      q.query[kv.substr(0, eq)] = kv.substr(eq + 1);
      if (amp == std::string::npos) break;
      pos = amp + 1;
    }
  }
  ApiResponse r = api.handle(q);
  CHECK_EQ(r.status, expect);
  JsonDocument d;
  deserializeJson(d, r.body);
  return d;
}
}  // namespace

TEST(stats_buckets_downsample_and_ring) {
  History h(8);  // tiny ring to test wrap-around
  uint32_t t0 = 29470000;  // epoch minutes, aligned to a full hour below
  t0 -= t0 % 60;
  for (uint32_t m = 0; m < 15 * 3; ++m) h.record("a", sample(20.f + m * 0.01f, 21.f, m % 2 ? 1.f : 0.f), t0 + m);
  h.record("a", sample(20.f, 21.f, 0.f), t0 + 45);  // opens the 4th bucket, flushes the 3rd
  auto pts = h.query("a", 24);
  CHECK_EQ(pts.size(), 3u);
  CHECK_EQ(pts[0].tMin, t0);
  CHECK_EQ(pts[0].minutes, 15);
  CHECK_EQ(pts[0].duty, 47);  // 7 of 15 minutes
  CHECK_NEAR(pts[0].temp / 100.f, 20.07f, 0.011f);
  CHECK_EQ(pts[1].tMin, t0 + 15);
  auto hourly = History::downsample(pts, 60);
  CHECK_EQ(hourly.size(), 1u);
  CHECK_EQ(hourly[0].minutes, 45);
  // ring keeps only the newest 8 buckets
  for (uint32_t m = 46; m < 15 * 20; ++m) h.record("a", sample(20.f, 21.f, 0.f), t0 + m);
  CHECK_EQ(h.points("a"), 8u);
  pts = h.query("a", 1000);
  CHECK_EQ(pts.back().tMin, t0 + 18 * 15);
  CHECK(pts.front().tMin < pts.back().tMin);
}

TEST(stats_kpis_warmup_overshoot_exclusions) {
  std::vector<HistPoint> pts;
  auto P = [&](float t, float sp, uint8_t duty, uint8_t flags = kHistComfortSlot, uint8_t win = 0) {
    HistPoint p;
    p.tMin = 1000 + static_cast<uint32_t>(pts.size()) * 15;
    p.temp = static_cast<int16_t>(t * 100);
    p.setpoint = static_cast<int16_t>(sp * 100);
    p.duty = duty;
    p.minutes = 15;
    p.flags = flags;
    p.windowMin = win;
    pts.push_back(p);
  };
  P(18.0f, 18.f, 0, 0);                 // eco
  P(18.2f, 21.f, 100);                  // step to comfort: warm-up starts
  P(19.0f, 21.f, 100);
  P(20.0f, 21.f, 100);
  P(20.8f, 21.f, 100);                  // >= 20.7 after 3 buckets -> 52.5 min
  P(21.6f, 21.f, 0);                    // overshoot 0.6 K, above setpoint
  P(21.3f, 21.f, 0);
  P(17.0f, 21.f, 0, kHistComfortSlot, 15);  // window open: excluded
  P(19.0f, 21.f, 100, kHistComfortSlot | kHistFault);  // fault: excluded
  RoomKpis k = History::computeKpis(pts);
  CHECK_EQ(k.warmups, 1u);
  CHECK_NEAR(k.warmupMeanMin, 52.5f, 0.01f);
  CHECK_NEAR(k.maxOvershootK, 0.6f, 0.011f);
  CHECK_NEAR(k.aboveHours, 0.25f, 0.001f);  // only the 21.6 bucket is > +0.5 K
  CHECK_NEAR(k.belowHours, 0.75f, 0.001f);  // 18.2, 19.0, 20.0 (window/fault excluded)
  CHECK_NEAR(k.heatingHours, 1.25f, 0.001f);
  CHECK_NEAR(k.windowHours, 0.25f, 0.001f);
  CHECK_NEAR(k.faultHours, 0.25f, 0.001f);
  CHECK(k.comfortMeanDev < 0.f);
}

TEST(stats_serialize_roundtrip_and_corruption) {
  History h;
  for (uint32_t m = 0; m < 600; ++m) {
    h.record("wohnen", sample(20.f + (m % 30) * 0.05f, 21.f, 0.5f), 29470000 + m);
    h.record(kHistSystemSeries, sample(35.f, 0.f, 0.3f), 29470000 + m);
  }
  std::string blob = h.serialize();
  History g;
  CHECK(g.deserialize(blob));
  CHECK_EQ(g.points("wohnen"), h.points("wohnen"));
  auto a = h.query("wohnen", 1000), b = g.query("wohnen", 1000);
  CHECK_EQ(a.size(), b.size());
  CHECK(!a.empty() && std::memcmp(a.data(), b.data(), a.size() * sizeof(HistPoint)) == 0);
  blob[blob.size() / 2] ^= 0x10;
  History c;
  CHECK(!c.deserialize(blob));
  CHECK(!c.deserialize("garbage"));
  // removed rooms disappear, the system series stays
  g.keepOnly({});
  CHECK_EQ(g.points("wohnen"), 0u);
  CHECK(g.points(kHistSystemSeries) > 0);
}

TEST(stats_recorded_by_system_and_api) {
  Rig r;
  r.basicRoom();
  r.load(ConfigSection::Heating, R"({"rooms":[{"id":"wohnen","name":"Wohnen","comfort":21,"eco":18,"frost":7,
     "mode":"auto","hysteresis":0.4}],"circuits":[{"n":1,"room":"wohnen","minOnSec":60,"minOffSec":60}]})");
  r.plants["wohnen"].T = 17.f;
  r.run(26u * 3600u * 1000u);  // Monday 00:00 .. Tuesday 02:00 (UTC clock, no TZ)
  size_t n = r.sys.history.points("wohnen");
  CHECK(n >= 100 && n <= 104);
  CHECK(r.sys.history.points(kHistSystemSeries) >= 100);
  auto pts = r.sys.history.query("wohnen", 24);
  int comfortBuckets = 0, heating = 0;
  for (auto& p : pts) {
    if (p.flags & kHistComfortSlot) ++comfortBuckets;
    if (p.duty) ++heating;
  }
  CHECK(comfortBuckets >= 30 && comfortBuckets <= 34);  // 06-08 + 16-22 = 8 h
  CHECK(heating > 10);
  RoomKpis k = History::computeKpis(r.sys.history.query("wohnen", 30));
  CHECK(k.warmups >= 2);  // 06:00 and 16:00 steps 18 -> 21
  CHECK(k.switches >= 2);
  CHECK(k.heatingHours > 1.f);

  ApiRouter api(r.sys);
  auto sum = get(api, "/api/stats?days=2");
  CHECK_STR(sum["rooms"][0]["id"] | "", "wohnen");
  CHECK(sum["rooms"][0]["days"].size() >= 1);
  CHECK(sum["rooms"][0]["total"]["heatingHours"].as<float>() > 1.f);
  auto ser = get(api, "/api/stats/wohnen?hours=24&res=60");
  CHECK_EQ(ser["resolutionMin"].as<int>(), 60);
  // hour-aligned groups: 24 h of 15-minute buckets cover 24 or 25 clock hours
  CHECK(ser["data"]["t"].size() >= 24u && ser["data"]["t"].size() <= 25u);
  CHECK_EQ(ser["data"]["temp"].size(), ser["data"]["t"].size());
  CHECK(ser["kpis"]["warmups"].as<int>() >= 1);
  // long periods are thinned out automatically (<= ~600 points)
  auto lng = get(api, "/api/stats/wohnen?hours=336&res=15");
  CHECK(lng["resolutionMin"].as<int>() >= 45);
  get(api, "/api/stats/nirgends", 404);
  get(api, "/api/stats/_system?hours=6");
}
