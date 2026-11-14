// Pre-heating (optimum start) for slow rooms such as underfloor heating.
#include "SimRig.h"
#include "hc/api/ApiRouter.h"

using namespace hc;
using sim::Rig;

namespace {

LocalTime at(int weekday, int hour, int minute) {
  LocalTime lt;
  lt.valid = true;
  lt.year = 2026;
  lt.month = 1;
  lt.day = 5 + weekday;
  lt.weekday = weekday;
  lt.hour = hour;
  lt.minute = minute;
  return lt;
}

// room "wohnen": comfort 21 / eco 19, comfort Mon-Fri 06:00-22:00, Sat-Sun 07:00-22:00
void setup(Rig& r, int preheatMaxMin) {
  char buf[640];
  std::snprintf(buf, sizeof(buf),
                R"({"rooms":[{"id":"wohnen","name":"Wohnen","comfort":21,"eco":19,"frost":7,"mode":"auto",
                   "preheatMaxMin":%d,"schedule":[["06:00-22:00"],["06:00-22:00"],["06:00-22:00"],["06:00-22:00"],
                   ["06:00-22:00"],["07:00-22:00"],["07:00-22:00"]]}],
                   "circuits":[{"n":1,"room":"wohnen","minOnSec":60,"minOffSec":60}]})",
                preheatMaxMin);
  r.load(ConfigSection::Heating, buf);
}

float sp(Rig& r, const LocalTime& lt) {
  Room* w = r.sys.heating.room("wohnen");
  return r.sys.heating.computeSetpoint(*w, lt, 5.f, 28.f, r.clock.ms);
}

}  // namespace

TEST(preheat_starts_early_by_learned_rate_and_latches) {
  Rig r;
  setup(r, 240);
  Room* w = r.sys.heating.room("wohnen");
  CHECK_EQ(w->preheatMaxMin, 240);
  w->hasTemp = true;
  w->temperature = 19.f;
  w->preheatRateKh = 1.0f;  // 2 K at 1 K/h + 30 min dead time = 150 min ahead
  w->preheatDelayMin = 30.f;
  CHECK_NEAR(sp(r, at(0, 3, 0)), 19.0, 0.01);  // 180 min ahead: not yet
  CHECK_STR(w->reason, "Zeitplan Absenkung");
  CHECK_EQ(w->preheatLeadMin, 150);
  CHECK_NEAR(sp(r, at(0, 3, 31)), 21.0, 0.01);  // 149 min ahead: pre-heat
  CHECK_STR(w->reason, "Vorheizen fuer Komfort ab 06:00");
  // warmer room: keeps pre-heating until the comfort period begins
  w->temperature = 20.8f;
  CHECK_NEAR(sp(r, at(0, 5, 0)), 21.0, 0.01);
  CHECK_NEAR(sp(r, at(0, 6, 0)), 21.0, 0.01);
  CHECK_STR(w->reason, "Zeitplan Komfort");
  CHECK_EQ(w->preheatFor, -1);
  // evening: comfort ends 22:00, the next start (Tue 06:00) is 8 h away
  CHECK_NEAR(sp(r, at(0, 22, 0)), 19.0, 0.01);
  // weekend start 07:00, slow room (0.25 K/h): limited to 240 min ahead
  w->temperature = 19.f;
  w->preheatRateKh = 0.25f;
  CHECK_NEAR(sp(r, at(4, 23, 0)), 19.0, 0.01);  // Friday night, next Sat 07:00
  CHECK_NEAR(sp(r, at(5, 2, 59)), 19.0, 0.01);
  CHECK_NEAR(sp(r, at(5, 3, 0)), 21.0, 0.01);
  CHECK_EQ(w->preheatLeadMin, 240);
  // room already warm: no pre-heating needed
  w->preheatFor = -1;
  w->temperature = 21.2f;
  CHECK_NEAR(sp(r, at(1, 5, 30)), 19.0, 0.01);
  // no learned rate: the full lead time
  w->temperature = 19.f;
  w->preheatRateKh = 0.f;
  CHECK_NEAR(sp(r, at(1, 2, 1)), 21.0, 0.01);
  // manual mode or window take precedence
  w->preheatFor = -1;
  w->windowSources.push_back("di:2");
  CHECK_NEAR(sp(r, at(1, 5, 0)), 7.0, 0.01);
  w->windowSources.clear();
}

TEST(preheat_off_and_config) {
  Rig r;
  setup(r, 0);
  Room* w = r.sys.heating.room("wohnen");
  w->hasTemp = true;
  w->temperature = 19.f;
  w->preheatRateKh = 1.f;
  CHECK_NEAR(sp(r, at(0, 5, 59)), 19.0, 0.01);  // off: normal schedule
  // saving without the field keeps the value; out of range is rejected
  setup(r, 180);
  ApiRouter api(r.sys);
  ApiRequest q;
  q.method = "PUT";
  q.path = "/api/heating";
  q.body = R"({"rooms":[{"id":"wohnen","name":"Wohnen","comfort":21,"eco":19,"frost":7}],
              "circuits":[{"n":1,"room":"wohnen"}]})";
  CHECK_EQ(api.handle(q).status, 200);
  CHECK_EQ(r.sys.heating.room("wohnen")->preheatMaxMin, 180);
  q.body = R"({"rooms":[{"id":"wohnen","name":"Wohnen","comfort":21,"eco":19,"frost":7,"preheatMaxMin":900}],
              "circuits":[{"n":1,"room":"wohnen"}]})";
  CHECK_EQ(api.handle(q).status, 400);
  // the system feeds the learned rate from the thermal model
  r.sys.sensors.update("ow1:01", 19.f, r.clock.ms);
  r.run(5000, false);
  CHECK(r.sys.heating.room("wohnen")->preheatRateKh >= 0.f);
}
