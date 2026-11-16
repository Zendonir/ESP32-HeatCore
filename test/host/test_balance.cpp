// Hydraulic balance assistant: valve open times per room -> flow advice.
#include "SimRig.h"
#include "hc/api/ApiRouter.h"
#include "hc/mcp/McpServer.h"
#include "hc/mqtt/MqttBridge.h"

using namespace hc;
using sim::Rig;

namespace {

constexpr uint32_t kBase = 29459520;  // 2026-01-05 00:00 UTC in epoch minutes

ApiResponse call(ApiRouter& api, const char* method, const char* path, const char* body = "") {
  ApiRequest r;
  r.method = method;
  r.path = path;
  r.body = body;
  return api.handle(r);
}

// one 15-minute bucket
HistPoint pt(uint32_t tMin, float temp, float sp, int duty, uint8_t flags = kHistComfortSlot, float outdoor = 2.f) {
  HistPoint p;
  p.tMin = tMin;
  p.temp = static_cast<int16_t>(std::lround(temp * 100.f));
  p.setpoint = static_cast<int16_t>(std::lround(sp * 100.f));
  p.outdoor = static_cast<int16_t>(std::lround(outdoor * 100.f));
  p.duty = static_cast<uint8_t>(duty);
  p.minutes = 15;
  p.flags = flags;
  return p;
}

// `hours` of steady comfort buckets starting at tMin
std::vector<HistPoint> steady(uint32_t tMin, float hours, float dev, int duty) {
  std::vector<HistPoint> v;
  for (int i = 0; i < static_cast<int>(hours * 4); ++i) v.push_back(pt(tMin + i * 15u, 21.f + dev, 21.f, duty));
  return v;
}

RoomBalance room(const char* id, float dev, int duty, float hours = 20.f) {
  return BalanceAnalyzer::analyzeRoom(id, steady(kBase, hours, dev, duty), 0);
}

// minute samples for a room over `days`: comfort 06:00-22:00 (21 degC), else 18 degC
void record(Rig& r, const std::string& id, int days, float dev, float duty, uint32_t from = kBase) {
  for (uint32_t m = 0; m < static_cast<uint32_t>(days) * 1440u; ++m) {
    uint32_t t = from + m;
    uint32_t mod = t % 1440u;
    bool comfort = mod >= 360 && mod < 1320;
    HistSample s;
    s.tempValid = s.setpointValid = s.outdoorValid = true;
    s.setpoint = comfort ? 21.f : 18.f;
    s.temp = s.setpoint + dev;
    s.outdoor = 3.f;
    s.duty = duty;
    s.flags = comfort ? kHistComfortSlot : 0;
    r.sys.history.record(id, s, t);
  }
}

}  // namespace

TEST(balance_analyzer_counts_only_steady_comfort_periods) {
  std::vector<HistPoint> v;
  uint32_t t = kBase;
  auto add = [&](int n, float temp, float sp, int duty, uint8_t flags, float outdoor = 2.f) {
    for (int i = 0; i < n; ++i, t += 15) v.push_back(pt(t, temp, sp, duty, flags, outdoor));
  };
  add(8, 18.f, 18.f, 0, 0);                        // night setback: not comfort
  add(8, 19.f, 21.f, 100, kHistComfortSlot);       // 2 h warm-up after the step: skipped
  add(8, 20.f, 21.f, 90, kHistComfortSlot);        // counted: 2 h, 1 K below, 90 %
  add(4, 20.f, 21.f, 0, kHistComfortSlot | kHistManualOutput);  // manual valve
  add(4, 20.f, 21.f, 0, kHistComfortSlot | kHistNoSupply);      // boiler gave no heat
  add(4, 20.f, 21.f, 0, kHistComfortSlot | kHistFault);
  add(4, 22.f, 21.f, 0, kHistComfortSlot, 18.f);   // mild weather (> 15 degC outside)
  HistPoint w = pt(t, 17.f, 21.f, 100);
  w.windowMin = 5;                                 // window open
  v.push_back(w);
  t += 15;
  add(4, 21.6f, 21.f, 50, kHistComfortSlot);       // counted: 1 h, 0.6 K above, 50 %
  RoomBalance rb = BalanceAnalyzer::analyzeRoom("x", v, 0);
  CHECK_NEAR(rb.hours, 3.0, 0.01);
  CHECK_NEAR(rb.duty, (90.0 * 2 + 50.0) / 3.0, 0.01);
  CHECK_NEAR(rb.dev, (-1.0 * 2 + 0.6) / 3.0, 0.01);
  CHECK_NEAR(rb.belowPct, 66.67, 0.1);
  CHECK_NEAR(rb.abovePct, 33.33, 0.1);
  CHECK_NEAR(rb.outdoor, 2.0, 0.01);
  // data before `since` is ignored (after a manifold adjustment)
  RoomBalance late = BalanceAnalyzer::analyzeRoom("x", v, t - 60);
  CHECK_NEAR(late.hours, 1.0, 0.01);
}

TEST(balance_classify_rooms_and_system) {
  BalanceResult r;
  r.rooms.push_back(room("kalt", -1.0f, 97));      // open all the time, too cold
  r.rooms.push_back(room("knapp", -0.1f, 88));     // open all the time, just holds
  r.rooms.push_back(room("normal", 0.f, 45));
  r.rooms.push_back(room("leicht", 0.1f, 12));     // holds with little valve time
  r.rooms.push_back(room("sonne", 0.9f, 30));      // overshoots
  r.rooms.push_back(room("neu", 0.f, 50, 6.f));    // too little data
  BalanceAnalyzer::classify(r);
  CHECK_EQ(r.assessed, 5);
  CHECK_NEAR(r.medianDuty, 45.0, 0.01);
  CHECK(r.rooms[0].verdict == BalanceVerdict::Increase);
  CHECK(r.rooms[1].verdict == BalanceVerdict::IncreaseSlight);
  CHECK(r.rooms[2].verdict == BalanceVerdict::Ok);
  CHECK(r.rooms[3].verdict == BalanceVerdict::Reduce);
  CHECK(r.rooms[4].verdict == BalanceVerdict::Reduce);
  CHECK(r.rooms[5].verdict == BalanceVerdict::NoData);
  CHECK(r.system == BalanceSystem::Ok);
  CHECK(BalanceAnalyzer::advice(r.rooms[0], r.system, true).find("alle Kreise") != std::string::npos);
  CHECK(BalanceAnalyzer::advice(r.rooms[5], r.system, false).find("6 von 12") != std::string::npos);
  // a room working much harder than the others gets a little more flow
  BalanceResult rel;
  rel.rooms.push_back(room("a", 0.f, 70));
  rel.rooms.push_back(room("b", 0.f, 30));
  rel.rooms.push_back(room("c", 0.f, 35));
  BalanceAnalyzer::classify(rel);
  CHECK(rel.rooms[0].verdict == BalanceVerdict::IncreaseSlight);
  CHECK(rel.rooms[1].verdict == BalanceVerdict::Ok);

  // most rooms short of heat, none with heat to spare: flow temperature too low
  BalanceResult low;
  low.rooms.push_back(room("a", -1.2f, 98));
  low.rooms.push_back(room("b", -0.8f, 95));
  low.rooms.push_back(room("c", -0.2f, 70));
  BalanceAnalyzer::classify(low);
  CHECK(low.system == BalanceSystem::FlowLow);
  CHECK(BalanceAnalyzer::advice(low.rooms[0], low.system, false).find("Vorlauftemperatur") != std::string::npos);

  // everything needs little valve time: the flow temperature could be lower;
  // only a clear outlier is still worth reducing
  BalanceResult high;
  high.rooms.push_back(room("a", 0.f, 30));
  high.rooms.push_back(room("b", 0.1f, 18));
  high.rooms.push_back(room("c", 0.2f, 8));
  BalanceAnalyzer::classify(high);
  CHECK(high.system == BalanceSystem::FlowHigh);
  CHECK(high.rooms[0].verdict == BalanceVerdict::Ok);
  CHECK(high.rooms[1].verdict == BalanceVerdict::Ok);
  CHECK(high.rooms[2].verdict == BalanceVerdict::Reduce);

  // external heat: warm with the valve hardly open
  RoomBalance k = room("kueche", 0.8f, 5);
  CHECK(k.externalHeat);
  BalanceResult one;
  one.rooms.push_back(k);
  BalanceAnalyzer::classify(one);
  CHECK(BalanceAnalyzer::advice(one.rooms[0], one.system, false).find("Fremdw") != std::string::npos);
  // nothing to assess
  BalanceResult none;
  BalanceAnalyzer::classify(none);
  CHECK(none.system == BalanceSystem::None);
}

TEST(balance_api_notes_persistence_and_mqtt) {
  Rig r;
  r.load(ConfigSection::Heating,
         R"({"rooms":[{"id":"flur","name":"Flur","comfort":21,"eco":18,"frost":7},
                      {"id":"wohnen","name":"Wohnzimmer","comfort":21,"eco":18,"frost":7},
                      {"id":"bad","name":"Bad","comfort":22,"eco":18,"frost":7},
                      {"id":"keller","name":"Keller","comfort":15,"eco":12,"frost":7}],
             "circuits":[{"n":1,"room":"flur"},{"n":2,"room":"wohnen"},{"n":3,"room":"wohnen","share":80},
                         {"n":4,"room":"bad"}]})");
  record(r, "flur", 2, -0.9f, 0.96f);
  record(r, "wohnen", 2, 0.f, 0.5f);
  record(r, "bad", 2, 0.1f, 0.1f);
  r.clock.advance(3ull * 86400ull * 1000ull);  // now: 3 days after kBase
  ApiRouter api(r.sys);
  JsonDocument d;
  CHECK_EQ(call(api, "GET", "/api/balance").status, 200);
  deserializeJson(d, call(api, "GET", "/api/balance").body);
  CHECK(d["timeValid"] == true);
  CHECK_EQ(d["days"].as<int>(), 7);
  CHECK_STR(std::string(d["system"] | ""), "ok");
  JsonArray rooms = d["rooms"];
  CHECK_EQ(rooms.size(), 3u);  // the cellar has no circuit
  CHECK_STR(std::string(rooms[0]["id"] | ""), "flur");
  CHECK_STR(std::string(rooms[0]["verdict"] | ""), "increase");
  CHECK_NEAR(rooms[0]["duty"].as<float>(), 96.0, 0.5);
  CHECK_NEAR(rooms[0]["hours"].as<float>(), 28.0, 0.5);  // 2 x (16 h comfort - 2 h warm-up)
  CHECK_STR(std::string(rooms[1]["verdict"] | ""), "ok");
  CHECK_EQ(rooms[1]["circuits"].size(), 2u);
  CHECK_EQ(rooms[1]["circuits"][1]["share"].as<int>(), 80);
  CHECK_STR(std::string(rooms[2]["verdict"] | ""), "reduce");
  CHECK_STR(std::string(rooms[2]["label"] | ""), "Durchfluss reduzieren");

  // HA: one sensor per room with circuits, state text in the room state
  {
    MqttBridge br(r.sys);
    std::vector<MqttMsg> msgs;
    br.discovery(msgs);
    int sensors = 0;
    for (auto& m : msgs)
      if (m.topic.find("_balance/config") != std::string::npos && !m.payload.empty()) ++sensors;
    CHECK_EQ(sensors, 3);
    std::vector<MqttMsg> st;
    br.state(st);
    std::string flur;
    for (auto& m : st)
      if (m.topic.find("/room/flur/state") != std::string::npos) {
        JsonDocument x;
        deserializeJson(x, m.payload);
        flur = x["balance"] | "";
        CHECK(std::string(x["balance_advice"] | "").find("erh") != std::string::npos);
      }
    CHECK_STR(flur, "Durchfluss erhöhen");
  }

  // MCP (AI assistants): read-only tool with the same analysis
  {
    r.load(ConfigSection::Ai, R"({"access":"read","token":"test-token-0123456789"})");
    McpServer mcp(r.sys);
    std::string out;
    mcp.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"get_balance","arguments":{}}})", out);
    JsonDocument x;
    deserializeJson(x, out);
    CHECK(x["result"]["isError"] == false);
    JsonDocument data;
    deserializeJson(data, std::string(x["result"]["content"][0]["text"] | ""));
    CHECK_STR(std::string(data["rooms"][0]["verdict"] | ""), "increase");
  }

  // change log: the adjusted room is assessed on new data only
  CHECK_EQ(call(api, "POST", "/api/balance/notes", R"({"room":"flur","text":"Kreis 1: 1,5 -> 2,0 l/min"})").status,
           200);
  CHECK_EQ(call(api, "POST", "/api/balance/notes", R"({"room":"gibtsnicht","text":"x"})").status, 400);
  CHECK_EQ(call(api, "POST", "/api/balance/notes", R"({"room":"flur","text":""})").status, 400);
  deserializeJson(d, call(api, "GET", "/api/balance").body);
  CHECK_STR(std::string(d["rooms"][0]["verdict"] | ""), "nodata");
  CHECK(d["rooms"][0]["afterNote"] == true);
  CHECK(std::string(d["rooms"][0]["advice"] | "").find("Nach der letzten") != std::string::npos);
  CHECK_STR(std::string(d["rooms"][1]["verdict"] | ""), "ok");  // other rooms unaffected
  CHECK_EQ(d["notes"].size(), 1u);
  CHECK_STR(std::string(d["notes"][0]["roomName"] | ""), "Flur");
  uint32_t t = d["notes"][0]["t"] | 0u;
  CHECK_EQ(t, kBase + 3u * 1440u);

  // after the settling day and enough new data the room is assessed again
  record(r, "flur", 2, -0.1f, 0.6f, kBase + 3u * 1440u);
  r.clock.advance(2ull * 86400ull * 1000ull);
  deserializeJson(d, call(api, "GET", "/api/balance").body);
  CHECK_STR(std::string(d["rooms"][0]["verdict"] | ""), "ok");
  CHECK_NEAR(d["rooms"][0]["hours"].as<float>(), 14.0, 0.5);  // only the second day counts

  // settings survive a save / load of the heating section; saving the rooms
  // without the balance block keeps it
  CHECK_EQ(call(api, "PUT", "/api/balance", R"({"days":10})").status, 200);
  CHECK_EQ(call(api, "PUT", "/api/balance", R"({"days":20})").status, 400);
  CHECK_EQ(call(api, "PUT", "/api/balance", R"({"notes":[]})").status, 400);
  JsonDocument saved;
  r.sys.sectionToJson(ConfigSection::Heating, saved.to<JsonObject>(), false);
  CHECK_EQ(saved["balance"]["days"].as<int>(), 10);
  CHECK_EQ(saved["balance"]["notes"].size(), 1u);
  r.sys.balanceCfg = BalanceConfig();
  CHECK_STR(r.sys.sectionFromJson(ConfigSection::Heating, saved.as<JsonObjectConst>()), "");
  CHECK_EQ(r.sys.balanceCfg.days, 10);
  CHECK_EQ(r.sys.balanceCfg.notes.size(), 1u);
  saved.remove("balance");
  CHECK_STR(r.sys.sectionFromJson(ConfigSection::Heating, saved.as<JsonObjectConst>()), "");
  CHECK_EQ(r.sys.balanceCfg.notes.size(), 1u);

  // delete the note
  std::string path = "/api/balance/notes/" + std::to_string(t);
  CHECK_EQ(call(api, "DELETE", path.c_str()).status, 200);
  CHECK_EQ(call(api, "DELETE", path.c_str()).status, 404);
  CHECK(r.sys.balanceCfg.notes.empty());

  // without time no note can be dated
  r.clock.epochBase = 0;
  CHECK_EQ(call(api, "POST", "/api/balance/notes", R"({"room":"","text":"alle"})").status, 409);
  deserializeJson(d, call(api, "GET", "/api/balance").body);
  CHECK(d["timeValid"] == false);
  CHECK_STR(std::string(d["system"] | ""), "none");
}
