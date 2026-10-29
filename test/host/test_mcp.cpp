#include "SimRig.h"
#include "hc/api/ApiRouter.h"
#include "hc/mcp/McpServer.h"

using namespace hc;
using sim::Rig;

namespace {

struct Mcp {
  Rig& r;
  McpServer srv;
  int id = 0;
  explicit Mcp(Rig& rig) : r(rig), srv(rig.sys) {}

  JsonDocument rpc(const char* method, const char* params = "{}", int* status = nullptr) {
    std::string body = std::string(R"({"jsonrpc":"2.0","id":)") + std::to_string(++id) + R"(,"method":")" + method +
                       R"(","params":)" + params + "}";
    std::string out;
    int st = srv.handle(body, out);
    if (status) *status = st;
    JsonDocument d;
    deserializeJson(d, out);
    return d;
  }
  // tools/call -> parsed JSON of the text content; isError reported via flag
  JsonDocument call(const char* tool, const char* args, bool* isError = nullptr) {
    std::string p = std::string(R"({"name":")") + tool + R"(","arguments":)" + args + "}";
    JsonDocument d = rpc("tools/call", p.c_str());
    bool err = d["result"]["isError"] | true;
    if (isError) *isError = err;
    JsonDocument out;
    std::string text = d["result"]["content"][0]["text"] | "";
    if (!err) deserializeJson(out, text);
    else out["error"] = text;
    return out;
  }
};

void setAccess(Rig& r, const char* access) {
  std::string j = std::string(R"({"access":")") + access + R"(","token":"test-token-0123456789"})";
  r.load(ConfigSection::Ai, j.c_str());
}

ApiResponse api(Rig& r, const char* method, const std::string& path, const char* body = "") {
  ApiRouter a(r.sys);
  ApiRequest q;
  q.method = method;
  q.path = path;
  q.body = body;
  return a.handle(q);
}

bool hasTool(JsonDocument& list, const char* name) {
  for (JsonObject t : list["result"]["tools"].as<JsonArray>())
    if (std::string(t["name"] | "") == name) return true;
  return false;
}

}  // namespace

TEST(mcp_protocol_handshake_and_errors) {
  Rig r;
  r.basicRoom();
  Mcp m(r);
  // disabled by default
  JsonDocument d = m.rpc("initialize", R"({"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"t","version":"1"}})");
  CHECK_EQ(d["error"]["code"].as<int>(), -32001);
  setAccess(r, "read");
  d = m.rpc("initialize", R"({"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"claude-code","version":"2.1"}})");
  CHECK_STR(d["jsonrpc"] | "", "2.0");
  CHECK_STR(d["result"]["protocolVersion"] | "", "2025-06-18");
  CHECK_STR(d["result"]["serverInfo"]["name"] | "", "heatcore");
  CHECK(!d["result"]["capabilities"]["tools"].isNull());
  CHECK(std::string(d["result"]["instructions"] | "").size() > 50);
  CHECK_STR(r.sys.ai.lastClient, "claude-code 2.1");
  // unknown version -> server proposes its latest
  d = m.rpc("initialize", R"({"protocolVersion":"2099-01-01","capabilities":{},"clientInfo":{"name":"x"}})");
  CHECK_STR(d["result"]["protocolVersion"] | "", McpServer::latestVersion());
  // stateless probe of newer clients -> method not found (client falls back to initialize)
  d = m.rpc("server/discover", R"({})");
  CHECK_EQ(d["error"]["code"].as<int>(), -32601);
  // notification: no body, HTTP 202
  std::string out;
  CHECK_EQ(m.srv.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})", out), 202);
  CHECK(out.empty());
  // ping, parse error, invalid request, batch
  d = m.rpc("ping");
  CHECK(d["result"].is<JsonObject>());
  CHECK_EQ(m.srv.handle("{kaputt", out), 400);
  CHECK(out.find("-32700") != std::string::npos);
  CHECK_EQ(m.srv.handle("42", out), 400);
  CHECK_EQ(m.srv.handle(R"([{"jsonrpc":"2.0","id":1,"method":"ping"},{"jsonrpc":"2.0","method":"notifications/x"}])", out), 200);
  JsonDocument b;
  deserializeJson(b, out);
  CHECK_EQ(b.size(), 1u);
  // prompts
  d = m.rpc("prompts/list");
  CHECK_STR(d["result"]["prompts"][0]["name"] | "", "optimize_heating");
  d = m.rpc("prompts/get", R"({"name":"optimize_heating","arguments":{"room":"wohnen"}})");
  CHECK(std::string(d["result"]["messages"][0]["content"]["text"] | "").find("wohnen") != std::string::npos);
  d = m.rpc("prompts/get", R"({"name":"nope"})");
  CHECK_EQ(d["error"]["code"].as<int>(), -32602);
}

TEST(mcp_read_tools) {
  Rig r;
  r.basicRoom();
  r.load(ConfigSection::Heating, R"({"rooms":[{"id":"wohnen","name":"Wohnen","comfort":21,"eco":18,"mode":"auto"}],
     "circuits":[{"n":1,"room":"wohnen","minOnSec":60,"minOffSec":60}]})");
  r.plants["wohnen"].T = 18.f;
  r.run(10u * 3600u * 1000u);
  setAccess(r, "read");
  Mcp m(r);
  JsonDocument list = m.rpc("tools/list");
  CHECK(hasTool(list, "get_statistics"));
  CHECK(!hasTool(list, "change_settings"));  // read-only access
  for (JsonObject t : list["result"]["tools"].as<JsonArray>()) CHECK(t["inputSchema"]["type"] == "object");
  bool err = true;
  JsonDocument o = m.call("get_overview", "{}", &err);
  CHECK(!err);
  CHECK_STR(o["rooms"][0]["id"] | "", "wohnen");
  CHECK_STR(o["aiAccess"] | "", "read");
  o = m.call("get_settings", "{}", &err);
  CHECK(!err);
  CHECK_EQ(o["rooms"][0]["comfort"].as<float>(), 21.f);
  CHECK_EQ(o["rooms"][0]["schedule"].size(), 7u);
  CHECK(!o["safetyReadOnly"]["maxFlowTemp"].isNull());
  CHECK(!o["aiLimits"]["room"]["comfort"].isNull());
  o = m.call("get_statistics", R"({"days":2})", &err);
  CHECK(!err);
  CHECK(o["rooms"][0]["total"]["heatingHours"].as<float>() > 0.f);
  o = m.call("get_room_history", R"({"room":"wohnen","hours":6,"resolution_min":60})", &err);
  CHECK(!err);
  CHECK(o["data"]["temp"].size() >= 6u);
  m.call("get_room_history", R"({"room":"keller"})", &err);
  CHECK(err);
  o = m.call("get_model", R"({"room":"wohnen"})", &err);
  CHECK(!err);
  CHECK(!o["rooms"][0]["status"].isNull());
  o = m.call("get_events", R"({"limit":10})", &err);
  CHECK(!err);
  o = m.call("get_change_log", "{}", &err);
  CHECK(!err);
  // writing is not possible with read access
  JsonDocument d = m.rpc("tools/call", R"({"name":"change_settings","arguments":{"changes":[{"type":"room","room":"wohnen","comfort":21.5}],"reason":"x"}})");
  CHECK_EQ(d["error"]["code"].as<int>(), -32602);
  CHECK_EQ(r.sys.heating.room("wohnen")->comfort, 21.f);
}

TEST(mcp_approve_flow_accept_and_revert) {
  Rig r;
  r.basicRoom();
  setAccess(r, "approve");
  Mcp m(r);
  JsonDocument list = m.rpc("tools/list");
  CHECK(hasTool(list, "change_settings"));
  bool err = true;
  const char* change = R"({"changes":[{"type":"room","room":"wohnen","comfort":21.5,"hysteresis":0.3},
      {"type":"schedule","room":"wohnen","days":[0,1,2,3,4],"periods":["16:00-22:00","05:30-08:00"]},
      {"type":"circuit","circuit":1,"minOnSec":300},{"type":"model","maxAdviceK":0.3}],
      "reason":"Aufheizen dauert im Mittel 35 min; Ueberschwingen 0,6 K","dry_run":true})";
  JsonDocument o = m.call("change_settings", change, &err);
  CHECK(!err);
  CHECK_STR(o["status"] | "", "dry_run");
  CHECK(std::string(o["summary"] | "").find("Komfort 21,0 -> 21,5") != std::string::npos);
  CHECK_EQ(r.sys.heating.room("wohnen")->comfort, 21.f);  // nothing changed
  CHECK(r.sys.ai.journal.empty());
  // real proposal -> pending, still nothing changed
  std::string real = change;
  real.replace(real.find("\"dry_run\":true"), 14, "\"dry_run\":false");
  o = m.call("change_settings", real.c_str(), &err);
  CHECK(!err);
  CHECK_STR(o["status"] | "", "pending");
  int id = o["id"] | 0;
  CHECK(id > 0);
  CHECK_EQ(r.sys.heating.room("wohnen")->comfort, 21.f);
  // the person accepts it in the web interface
  ApiResponse a = api(r, "POST", "/api/ai/changes/" + std::to_string(id) + "/accept");
  CHECK_EQ(a.status, 200);
  const Room* w = r.sys.heating.room("wohnen");
  CHECK_NEAR(w->comfort, 21.5f, 0.001f);
  CHECK_NEAR(w->hysteresis, 0.3f, 0.001f);
  CHECK_EQ(w->schedule[0].size(), 2u);
  CHECK_EQ(w->schedule[0][0].start, 5 * 60 + 30);  // sorted
  CHECK_EQ(w->schedule[5].size(), 1u);              // weekend untouched
  CHECK_EQ(r.sys.heating.circuits[0].minOnSec, 300);
  CHECK_NEAR(r.sys.model.cfg.maxAdviceK, 0.3f, 0.001f);
  CHECK_STR(r.sys.ai.find(static_cast<uint16_t>(id))->status, "applied");
  CHECK_EQ(api(r, "POST", "/api/ai/changes/" + std::to_string(id) + "/accept").status, 409);  // not pending anymore
  // revert through MCP restores everything
  o = m.call("revert_change", (R"({"id":)" + std::to_string(id) + R"(,"reason":"Test"})").c_str(), &err);
  CHECK(!err);
  CHECK_EQ(r.sys.heating.room("wohnen")->comfort, 21.f);
  CHECK_NEAR(r.sys.heating.room("wohnen")->hysteresis, 0.4f, 0.001f);
  CHECK_EQ(r.sys.heating.room("wohnen")->schedule[0].size(), 2u);
  CHECK_EQ(r.sys.heating.room("wohnen")->schedule[0][0].start, 6 * 60);
  CHECK_EQ(r.sys.heating.circuits[0].minOnSec, 60);
  CHECK_NEAR(r.sys.model.cfg.maxAdviceK, 0.5f, 0.001f);
  m.call("revert_change", (R"({"id":)" + std::to_string(id) + "}").c_str(), &err);
  CHECK(err);  // only once
  // a rejected proposal changes nothing
  o = m.call("change_settings", R"({"changes":[{"type":"room","room":"wohnen","eco":17}],"reason":"Energie"})", &err);
  id = o["id"] | 0;
  CHECK_EQ(api(r, "POST", "/api/ai/changes/" + std::to_string(id) + "/reject").status, 200);
  CHECK_EQ(r.sys.heating.room("wohnen")->eco, 18.f);
  // journal is visible to the AI and to the web UI
  o = m.call("get_change_log", "{}", &err);
  CHECK_EQ(o["changes"].size(), 2u);
  CHECK_STR(o["changes"][0]["status"] | "", "rejected");
  ApiResponse g = api(r, "GET", "/api/ai");
  CHECK(g.body.find("\"tokenSet\":true") != std::string::npos);
  CHECK(g.body.find("test-token-0123456789") == std::string::npos);  // token never shown
}

TEST(mcp_limits_protect_safety_and_comfort) {
  Rig r;
  r.basicRoom();
  setAccess(r, "direct");
  Mcp m(r);
  bool err = false;
  auto bad = [&](const char* args, const char* expect) {
    JsonDocument o = m.call("change_settings", args, &err);
    CHECK(err);
    std::string e = o["error"] | "";
    if (e.find(expect) == std::string::npos) std::printf("    unexpected error text: %s\n", e.c_str());
    CHECK(e.find(expect) != std::string::npos);
  };
  bad(R"({"changes":[{"type":"room","room":"wohnen","comfort":25}],"reason":"x"})", "ausserhalb");
  bad(R"({"changes":[{"type":"room","room":"wohnen","comfort":23}],"reason":"x"})", "max. 1,5 K");
  bad(R"({"changes":[{"type":"room","room":"wohnen","eco":21.5}],"reason":"x"})", "ausserhalb");
  bad(R"({"changes":[{"type":"room","room":"wohnen","eco":20,"comfort":19.6}],"reason":"x"})", "Ungueltig");
  bad(R"({"changes":[{"type":"room","room":"wohnen","maxFlowTemp":80}],"reason":"x"})", "nicht erlaubt");
  bad(R"({"changes":[{"type":"safety","maxFlowTemp":80}],"reason":"x"})", "unbekannter Typ");
  bad(R"({"changes":[{"type":"room","room":"keller","comfort":21}],"reason":"x"})", "Raum unbekannt");
  bad(R"({"changes":[{"type":"schedule","room":"wohnen","days":[0],"periods":["06:00-09:00","08:00-10:00"]}],"reason":"x"})", "ueberschneiden");
  bad(R"({"changes":[{"type":"schedule","room":"wohnen","days":[0],"periods":[]}],"reason":"x"})", "max. 180 min");
  bad(R"({"changes":[{"type":"schedule","room":"wohnen","days":[7],"periods":["06:00-08:00"]}],"reason":"x"})", "0..6");
  bad(R"({"changes":[{"type":"circuit","circuit":2,"minOnSec":300}],"reason":"x"})", "keinem Raum");
  bad(R"({"changes":[{"type":"circuit","circuit":1,"minOnSec":10}],"reason":"x"})", "60..1800");
  bad(R"({"changes":[{"type":"model","maxAdviceK":2}],"reason":"x"})", "maxAdviceK");
  bad(R"({"changes":[{"type":"room","room":"wohnen","comfort":21.5}],"reason":""})", "Begruendung");
  // safety maximum setpoint caps the comfort range
  r.sys.safety.cfg.maxSetpoint = 21.5f;
  bad(R"({"changes":[{"type":"room","room":"wohnen","comfort":22}],"reason":"x"})", "ausserhalb");
  CHECK_EQ(r.sys.heating.room("wohnen")->comfort, 21.f);  // nothing applied by any of these
  // a mixed request is all-or-nothing
  bad(R"({"changes":[{"type":"room","room":"wohnen","comfort":21.5},{"type":"circuit","circuit":1,"minOnSec":5}],"reason":"x"})", "60..1800");
  CHECK_EQ(r.sys.heating.room("wohnen")->comfort, 21.f);
  CHECK(r.sys.ai.journal.empty());
}

TEST(mcp_direct_mode_rate_limit_and_manual_change_guard) {
  Rig r;
  r.basicRoom();
  setAccess(r, "direct");
  r.load(ConfigSection::Ai, R"({"maxDirectPerDay":2})");
  Mcp m(r);
  bool err = true;
  JsonDocument o = m.call("change_settings", R"({"changes":[{"type":"room","room":"wohnen","comfort":21.5}],"reason":"a"})", &err);
  CHECK(!err);
  CHECK_STR(o["status"] | "", "applied");
  CHECK_NEAR(r.sys.heating.room("wohnen")->comfort, 21.5f, 0.001f);
  int first = o["id"] | 0;
  o = m.call("change_settings", R"({"changes":[{"type":"room","room":"wohnen","hysteresis":0.5}],"reason":"b"})", &err);
  CHECK(!err);
  o = m.call("change_settings", R"({"changes":[{"type":"room","room":"wohnen","hysteresis":0.6}],"reason":"c"})", &err);
  CHECK(err);  // daily limit
  CHECK(std::string(o["error"] | "").find("Tageslimit") != std::string::npos);
  // the person changes comfort in the web UI: reverting the AI change must not overwrite it
  r.load(ConfigSection::Heating, R"({"rooms":[{"id":"wohnen","name":"Wohnen","comfort":22,"eco":18,"hysteresis":0.5}],
     "circuits":[{"n":1,"room":"wohnen"}]})");
  o = m.call("revert_change", (R"({"id":)" + std::to_string(first) + "}").c_str(), &err);
  CHECK(err);
  CHECK(std::string(o["error"] | "").find("inzwischen") != std::string::npos);
  CHECK_EQ(r.sys.heating.room("wohnen")->comfort, 22.f);
  // events mark the source
  r.run(500, false);
  bool seen = false;
  for (auto& e : r.sys.bus.history(0, 200))
    if (e.source == EventSource::Ai && e.type == EventType::ConfigChanged) seen = true;
  CHECK(seen);
}

TEST(mcp_ai_config_and_journal_persist) {
  Rig a;
  a.basicRoom();
  setAccess(a, "approve");
  Mcp m(a);
  bool err = true;
  m.call("change_settings", R"({"changes":[{"type":"room","room":"wohnen","eco":17.5}],"reason":"Nacht kuehler"})", &err);
  CHECK(!err);
  JsonDocument d;
  a.sys.sectionToJson(ConfigSection::Ai, d.to<JsonObject>(), true);
  std::string s;
  serializeJson(d, s);
  Rig b;
  b.basicRoom();
  b.load(ConfigSection::Ai, s.c_str());
  CHECK(b.sys.ai.cfg.access == AiAccess::Approve);
  CHECK_STR(b.sys.ai.cfg.token, "test-token-0123456789");
  CHECK_EQ(b.sys.ai.journal.size(), 1u);
  CHECK_STR(b.sys.ai.journal[0].status, "pending");
  CHECK(b.sys.ai.journal[0].changes.find("17.5") != std::string::npos);
  // accepting after the restart works with the stored proposal
  CHECK_EQ(api(b, "POST", "/api/ai/changes/1/accept").status, 200);
  CHECK_NEAR(b.sys.heating.room("wohnen")->eco, 17.5f, 0.001f);
  // configuration rules
  JsonDocument x;
  deserializeJson(x, R"({"access":"approve","token":""})");
  CHECK(!b.sys.sectionFromJson(ConfigSection::Ai, x.as<JsonObjectConst>()).empty());  // token required
  deserializeJson(x, R"({"access":"root"})");
  CHECK(!b.sys.sectionFromJson(ConfigSection::Ai, x.as<JsonObjectConst>()).empty());
  deserializeJson(x, R"({"token":"kurz"})");
  CHECK(!b.sys.sectionFromJson(ConfigSection::Ai, x.as<JsonObjectConst>()).empty());
  // the export never contains the token
  JsonDocument ex;
  b.sys.sectionToJson(ConfigSection::Ai, ex.to<JsonObject>(), false);
  CHECK_STR(ex["token"] | "", kSecretMask);
}
